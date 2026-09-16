#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/fs.h"

#include <errno.h>
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
 * `stat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. An embedded `NUL` is rejected here too, so every caller may treat the
 * result as a C string.
 *
 * @param data       Buffer of at least `data_len` bytes that receives the file contents. Must not
 *                   be `NULL`.
 * @param data_len   Number of bytes to read, taken from the `stat` that preceded the open.
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

/**
 * @brief Writes all of `data` to `fp` without closing it.
 *
 * @param fp         Destination stream. Must not be `NULL`.
 * @param data       Source bytes. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a write failure.
 */
static int fs_write_file_bytes(FILE* fp,
                               const char* data,
                               size_t data_len,
                               char* reason,
                               size_t reason_len) __attribute__((nonnull(1, 2)));

int fs_read_file(const char* file_path,
                 char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) {
  struct stat st;
  if (stat(file_path, &st) != 0) {
    return fs_reason_errno(reason, reason_len, errno);
  }
  if (!S_ISREG(st.st_mode)) {
    return error_report(reason, reason_len, "not a regular file");
  }
  if (st.st_size < 0) {
    return error_report(reason, reason_len, "has a negative size");
  }
  // Reserve one byte for the terminator the allocation below adds, so `size + 1` cannot wrap to 0
  // and hand back a buffer shorter than the read. Both sides widen to `uintmax_t` because the
  // comparison only binds where `off_t` is wider than `size_t`, as on a 32-bit target.
  if ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX - 1) {
    return error_report(reason, reason_len, "exceeds max readable size (%zu bytes) at %ju bytes",
                        SIZE_MAX - 1, (uintmax_t)st.st_size);
  }

  const size_t size = (size_t)st.st_size;
  FILE* fp = fopen(file_path, "rb");
  if (fp == NULL) {
    return fs_reason_errno(reason, reason_len, errno);
  }

  // Use `malloc`, not `calloc`. `fs_read_file_bytes` writes all `size` bytes or fails, so only the
  // terminator needs to be zero, and zero-filling first would mean a second pass over the largest
  // file this program reads.
  char* data = malloc(size + 1);
  int rc = -1;
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
  const size_t directory_len = basename == NULL ? 0 : (size_t)(basename - destination_path) + 1;
  if (directory_len > SIZE_MAX - sizeof(suffix)) {
    free(resolved_path);
    return error_report(reason, reason_len, "path is too long");
  }

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

  int fd = -1;
  FILE* fp = NULL;
  int rc = -1;
  bool is_renamed = false;

  fd = mkstemp(temporary_path);
  if (fd < 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    goto cleanup;
  }
  fp = fdopen(fd, "wb");
  if (fp == NULL) {
    (void)fs_reason_errno(reason, reason_len, errno);
    goto cleanup;
  }
  fd = -1;

  rc = fs_write_file_bytes(fp, data, data_len, reason, reason_len);
  if (rc == 0 && has_existing_file && fchmod(fileno(fp), st.st_mode & 07777) != 0) {
    (void)fs_reason_errno(reason, reason_len, errno);
    rc = -1;
  }
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)fs_reason_errno(reason, reason_len, errno);
    }
    rc = -1;
  }
  fp = NULL;
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
  if (fp != NULL) {
    (void)fclose(fp);
  }
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
  const size_t nread = fread(data, 1, data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)fs_reason_errno(reason, reason_len, read_errno);
  } else if (nread != data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `stat`.
    (void)error_report(reason, reason_len, "shrank while being read");
  } else if (feof(fp) != 0) {
    // The read already consumed the whole file, so it cannot have grown.
    rc = 0;
  } else {
    // `fread` stops at `data_len`, so a file that grew between the caller's `stat` and here would
    // read as a silently truncated copy. One more byte tells the two apart. Nothing left to read
    // means the size still matches, while any byte at all means the file changed underneath us.
    char extra = 0;
    const size_t extra_read = fread(&extra, 1, 1, fp);
    const int extra_errno = errno;
    if (ferror(fp) != 0) {
      (void)fs_reason_errno(reason, reason_len, extra_errno);
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

static int fs_reason_errno(char* reason, size_t reason_len, int error_number) {
  char message[FS_REASON_SIZE];
  return error_report(reason, reason_len, "%s",
                      error_system_message(message, sizeof(message), error_number));
}

static int fs_write_file_bytes(FILE* fp,
                               const char* data,
                               size_t data_len,
                               char* reason,
                               size_t reason_len) {
  errno = 0;
  const size_t written = fwrite(data, 1, data_len, fp);
  const int write_errno = errno;
  if (written == data_len) {
    return 0;
  }
  if (ferror(fp) != 0) {
    return fs_reason_errno(reason, reason_len, write_errno == 0 ? EIO : write_errno);
  }
  return error_report(reason, reason_len, "wrote only %zu of %zu bytes", written, data_len);
}
