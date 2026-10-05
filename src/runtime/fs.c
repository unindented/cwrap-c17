#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/error.h"

/**
 * @brief Reads exactly `data_len` bytes into `data`, verifying the file did not change underneath.
 *
 * `fstat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. An embedded `NUL` is rejected here too, so every caller may treat the
 * result as a C string.
 *
 * @param data       Buffer of at least `data_len` bytes that receives the file contents. Must not
 *                   be `NULL`.
 * @param data_len   Number of bytes to read, taken from the `fstat` of the open file.
 * @param fp         Stream positioned at the start of the file. Must not be `NULL`.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` when exactly `data_len` stable, `NUL`-free bytes were read, or `-1` otherwise.
 */
static int fs_read_file_bytes(char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes every byte to an open descriptor, retrying interrupted and short writes.
 *
 * @param fd         Descriptor open for writing.
 * @param data       Bytes to write. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` once all `data_len` bytes are written, or `-1` on a write error or a write that makes
 *         no progress.
 */
static int write_all(int fd, const void* data, size_t data_len, char* reason, size_t reason_len)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the permission bits a newly created file gets from the process umask.
 *
 * `mkstemp` creates its file `0600` regardless of the umask, so a destination that did not exist
 * before needs the mode `fopen(path, "wb")` would have given it applied explicitly. There is no
 * portable call that only reads the umask, so this sets it and restores it. That flip is
 * process-wide and races with any other thread that creates a file, which is safe only because
 * cwrap is single-threaded. Revisit this before any file write moves off the main thread.
 *
 * @return `0666` reduced by the process umask.
 */
static mode_t fs_write_file_created_mode(void);

/**
 * @brief Reports `error_number` alone as a failure reason.
 *
 * This is for a failure on the path the caller already names, where repeating it would only pad the
 * composed diagnostic.
 *
 * @param reason       Receives the reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return fs_reason_errno(...);`.
 */
static int fs_reason_errno(char* reason, size_t reason_len, int error_number);

int fs_read_file(const char* file_path,
                 size_t data_len_max,
                 char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) {
  // Open without blocking and inspect the descriptor rather than the path, so the file checked is
  // the file read. A path swapped to a FIFO would otherwise block the open waiting for a writer.
  // The flag is cleared again once the file is known to be regular.
  const int fd = open(file_path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  struct stat st;
  int rc = 0;
  if (fstat(fd, &st) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  } else if (!S_ISREG(st.st_mode)) {
    rc = error_report(reason, reason_len, "not a regular file");
  } else if ((uintmax_t)st.st_size > (uintmax_t)data_len_max) {
    // Check the limit against the `fstat` size, before anything is allocated or read, so an
    // oversize file costs one `fstat` rather than its whole size in memory. A file that grows past
    // the limit after this check is still caught, because `fs_read_file_bytes` rejects any byte
    // beyond this size. `data_len_max` is below `SIZE_MAX` by contract, so passing this check also
    // keeps `size + 1` from wrapping to 0. Both sides widen to `uintmax_t` because `off_t` and
    // `size_t` may differ in width.
    rc = error_report(reason, reason_len, "exceeds max file size (%zu bytes) at %ju bytes",
                      data_len_max, (uintmax_t)st.st_size);
  }
  if (rc == 0 && fcntl(fd, F_SETFL, 0) != 0) {
    rc = fs_reason_errno(reason, reason_len, errno);
  }
  FILE* fp = rc == 0 ? fdopen(fd, "rb") : NULL;
  if (fp == NULL) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }

  const size_t size = (size_t)st.st_size;

  // Use `malloc`, not `calloc`. `fs_read_file_bytes` writes all `size` bytes or fails, so only the
  // terminator needs to be zero, and zero-filling first would mean a second pass over the largest
  // file this program reads.
  char* data = malloc(size + 1);
  rc = -1;
  if (data == NULL) {
    (void)error_report(reason, reason_len, "out of memory");
  } else {
    data[size] = '\0';
    rc = fs_read_file_bytes(data, size, fp, reason, reason_len);
  }
  // Close before publishing so a close error fails the read and the buffer is still reclaimed.
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  if (rc == 0) {
    *data_out = data;
    // `fs_read_file_bytes` succeeds only on a full read, so the byte count is the stat size.
    *data_len_out = size;
    data = NULL;
  }

  free(data);
  return rc;
}

int fs_write_file(const char* file_path,
                  const char* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) {
  char* resolved_path = NULL;
  struct stat link_st;
  if (lstat(file_path, &link_st) == 0 && S_ISLNK(link_st.st_mode)) {
    resolved_path = realpath(file_path, NULL);
    if (resolved_path == NULL) {
      return fs_reason_errno(reason, reason_len, errno);
    }
  }
  const char* destination_path = resolved_path == NULL ? file_path : resolved_path;

  static const char suffix[] = ".cwrap.XXXXXX";
  const char* basename = strrchr(destination_path, '/');
  // The directory is a prefix of a path already in memory, so adding the suffix cannot overflow.
  const size_t directory_len = basename == NULL ? 0 : (size_t)(basename - destination_path) + 1;

  char* temporary_path = malloc(directory_len + sizeof(suffix));
  if (temporary_path == NULL) {
    free(resolved_path);
    return error_report(reason, reason_len, "out of memory");
  }
  if (directory_len > 0) {
    memcpy(temporary_path, destination_path, directory_len);
  }
  memcpy(temporary_path + directory_len, suffix, sizeof(suffix));

  struct stat st;
  const bool has_existing_file = stat(destination_path, &st) == 0;
  if (!has_existing_file && errno != ENOENT) {
    const int stat_errno = errno;
    free(temporary_path);
    free(resolved_path);
    return fs_reason_errno(reason, reason_len, stat_errno);
  }

  const mode_t mode = has_existing_file ? st.st_mode & 07777 : fs_write_file_created_mode();

  int fd = -1;
  int rc = -1;
  bool is_renamed = false;

  fd = mkstemp(temporary_path);
  if (fd < 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    goto cleanup;
  }

  rc = write_all(fd, data, data_len, reason, reason_len);
  if (rc == 0 && fchmod(fd, mode) != 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    rc = -1;
  }
  // `fsync` before the `rename`, not after. Otherwise the new directory entry can reach the disk
  // while the bytes behind it are still in the page cache, and a power loss leaves the user's
  // source file empty or truncated in place of the previous complete one.
  if (rc == 0 && fsync(fd) != 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    rc = -1;
  }
  if (close(fd) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  fd = -1;
  if (rc != 0) {
    goto cleanup;
  }
  if (rename(temporary_path, destination_path) != 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    rc = -1;
    goto cleanup;
  }
  is_renamed = true;
  rc = 0;

cleanup:
  if (fd >= 0) {
    (void)close(fd);
  }
  if (!is_renamed) {
    (void)unlink(temporary_path);
  }
  free(temporary_path);
  free(resolved_path);
  return rc;
}

static int fs_read_file_bytes(char* data,
                              size_t data_len,
                              FILE* fp,
                              char* reason,
                              size_t reason_len) {
  // Reset `errno` so a value left by an earlier call cannot pass for the cause of this one. ISO C
  // does not require `fread` to set it, so a stream error that left it at 0 is reported as `EIO`.
  errno = 0;
  const size_t nread = fread(data, 1, data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)fs_reason_errno(reason, reason_len, read_errno == 0 ? EIO : read_errno);
  } else if (nread != data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `fstat`.
    (void)error_report(reason, reason_len, "shrank while being read");
  } else if (feof(fp) != 0) {
    // The read already consumed the whole file, so it cannot have grown.
    rc = 0;
  } else {
    // `fread` stops at `data_len`, so a file that grew between the caller's `fstat` and here would
    // read as a silently truncated copy. One more byte tells the two apart. Nothing left to read
    // means the size still matches, while any byte at all means the file changed underneath us.
    char extra = 0;
    errno = 0;
    const size_t extra_read = fread(&extra, 1, 1, fp);
    const int extra_errno = errno;
    if (ferror(fp) != 0) {
      (void)fs_reason_errno(reason, reason_len, extra_errno == 0 ? EIO : extra_errno);
    } else if (extra_read == 0) {
      rc = 0;
    } else {
      (void)error_report(reason, reason_len, "grew while being read");
    }
  }
  // Text is the only thing this program reads. Payload helpers use terminated strings, so a `NUL`
  // here could make classification or reconstruction silently ignore the remaining bytes.
  if (rc == 0 && memchr(data, '\0', nread) != NULL) {
    (void)error_report(reason, reason_len, "contains an embedded NUL byte");
    rc = -1;
  }
  return rc;
}

static int write_all(int fd, const void* data, size_t data_len, char* reason, size_t reason_len) {
  const unsigned char* bytes = data;
  size_t offset = 0;
  while (offset < data_len) {
    const ssize_t nwritten = write(fd, bytes + offset, data_len - offset);
    if (nwritten > 0) {
      offset += (size_t)nwritten;
    } else if (nwritten < 0 && errno == EINTR) {
      continue;
    } else if (nwritten < 0) {
      return fs_reason_errno(reason, reason_len, errno);
    } else {
      return error_report(reason, reason_len, "write made no progress");
    }
  }
  return 0;
}

static mode_t fs_write_file_created_mode(void) {
  const mode_t mask = umask(0);
  (void)umask(mask);
  return (mode_t)(0666 & ~mask);
}

static int fs_reason_errno(char* reason, size_t reason_len, int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "%s",
                      error_system_message(message, sizeof(message), error_number));
}
