#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "runtime/fs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Bytes the standard input buffer grows by before each `read`. Each step makes that much more of
 * the arena usable, so a larger step makes fewer calls into the arena, and a smaller one leaves
 * fewer unused bytes past the end of a short input.
 */
enum { OS_READ_STDIN_STEP_LEN = 64 * 1024 };

/**
 * The POSIX host. It has no state of its own: the process's working directory, umask and file
 * descriptors are the state, and every call reaches them through the system.
 */
struct Os {
  /** Unused. ISO C does not allow a structure without members. */
  char unused;
};

/**
 * Where an action reports its failure: the caller's log, and the caller's operation, which leads
 * every message so the user reads what failed before why, as in
 * `failed to read file: No such file or directory ('src/a.c')`.
 */
struct FsReport {
  /** Operation the caller was performing, such as `failed to read file`. */
  struct Str operation;

  /** Log that receives the message. */
  struct Log* log;
};

/**
 * @brief Opens a path without blocking and inspects the opened descriptor, the first step of
 *        `os_read`.
 *
 * Inspecting the descriptor rather than the path makes the file checked the file read. A path
 * swapped to a FIFO would otherwise block the open waiting for a writer.
 *
 * @param file_path Path to open.
 * @param fd_out    Receives the descriptor, still non-blocking. Written only on success. Must not
 *                  be `NULL`.
 * @param st_out    Receives the descriptor's status. Written only on success. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set and nothing left open when the path does not
 *         open or cannot be inspected.
 */
static int open_inspect(struct Str file_path, int* fd_out, struct stat* st_out)
    __attribute__((nonnull(2, 3)));

/**
 * @brief Checks what `open_inspect` saw against the read's limits, the second step of `os_read`.
 *
 * It looks only at the descriptor's `fstat`, so it runs before anything is allocated or read.
 *
 * @param st           Status of the opened descriptor. Must not be `NULL`.
 * @param file_path    Path of the file, named in a failure.
 * @param data_len_max Largest accepted file size in bytes.
 * @param report       Where a failure is reported. Must not be `NULL`.
 * @return `0` when the file is regular, within `data_len_max`, or `-1` after reporting which of the
 *         two it is not.
 */
static int os_read_check(const struct stat* st,
                         struct Str file_path,
                         ptrdiff_t data_len_max,
                         const struct FsReport* report) __attribute__((nonnull(1, 4)));

/**
 * @brief Reads the whole of a checked file into `data` and closes it, the third step of `os_read`.
 *
 * `open_inspect` left the descriptor non-blocking, and the flag is cleared here, now that the file
 * is known to be regular. The read goes through a stream over `fd`, which is closed whatever
 * happens, so a close error fails the read too. Only the first failure is reported, because a later
 * one is a consequence of it.
 *
 * @param data      Buffer of at least `data_len` bytes that receives the file contents. Must not be
 *                  `NULL`.
 * @param data_len  Number of bytes to read, taken from the `fstat` of the open file. Must not be
 *                  negative.
 * @param fd        Descriptor positioned at the start of the file, which this closes.
 * @param file_path Path of the file, named in a failure.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when exactly `data_len` stable, `NUL`-free bytes were read and the file closed, or
 *         `-1` otherwise.
 */
static int os_read_bytes(char* data,
                         ptrdiff_t data_len,
                         int fd,
                         struct Str file_path,
                         const struct FsReport* report) __attribute__((nonnull(1, 5)));

/**
 * @brief Reads exactly `data_len` bytes from a stream, verifying the file did not change
 *        underneath. A step of `os_read_bytes`.
 *
 * `fstat` and `fread` are two observations of a file another process may be writing, so a read that
 * returns anything other than `data_len` bytes is a changed file rather than a partial success.
 * Both directions are rejected: too few bytes means the file shrank, and one readable byte past
 * `data_len` means it grew. An embedded `NUL` is rejected here too.
 *
 * @param data      Buffer of at least `data_len` bytes that receives the file contents. Must not be
 *                  `NULL`.
 * @param data_len  Number of bytes to read. Must not be negative.
 * @param fp        Stream positioned at the start of the file. Must not be `NULL`.
 * @param file_path Path of the file, named in a failure.
 * @param report    Where a failure is reported. Must not be `NULL`.
 * @return `0` when exactly `data_len` stable, `NUL`-free bytes were read, or `-1` otherwise.
 */
static int os_read_bytes_exact(char* data,
                               ptrdiff_t data_len,
                               FILE* fp,
                               struct Str file_path,
                               const struct FsReport* report) __attribute__((nonnull(1, 3, 5)));

/**
 * @brief Reports `error_number` as the cause of a failed read of standard input.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return os_read_stdin_report_errno(...);`.
 */
static int os_read_stdin_report_errno(const struct FsReport* report, int error_number)
    __attribute__((nonnull(1)));

/**
 * @brief Resolves a destination that is a symbolic link to the file the link names, in place. A
 *        step of `os_write`.
 *
 * The write replaces the file a link names rather than the link, so the link stays a link. A path
 * that is not a link, or does not exist, is left as it is.
 *
 * @param path Terminated destination path, replaced by the resolved path when it names a symbolic
 *             link. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set when the link cannot be resolved: a dangling
 *         link fails with the `ENOENT` that `realpath` reports, and a resolved path of `PATH_MAX`
 *         bytes or more with `ENAMETOOLONG`.
 */
static int os_write_resolve(char path[static PATH_MAX]);

/**
 * @brief Builds the path of the sibling temporary file a write goes to. A step of `os_write`.
 *
 * @param destination_path Terminated path the write replaces. Must not be `NULL`.
 * @param temporary_out    Buffer of `PATH_MAX` bytes that receives the terminated `mkstemp`
 *                         template, in the destination's directory. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set to `ENAMETOOLONG` when the template would not
 *         fit in `PATH_MAX` bytes, as the system would fail it.
 */
static int os_write_temporary_path(const char destination_path[static 1],
                                   char temporary_out[static PATH_MAX]);

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
static mode_t os_write_created_mode(void);

/**
 * Most bytes `write_all` passes to one `write`, because macOS refuses a count above `INT_MAX` with
 * `EINVAL`.
 */
enum { FS_WRITE_LEN_MAX = 1024 * 1024 * 1024 };

/**
 * @brief Writes every byte to an open descriptor, retrying interrupted and short writes.
 *
 * @param fd   Descriptor open for writing.
 * @param data Bytes to write.
 * @return `0` once every byte is written, or `-1` with `errno` set on a write error, or to `EIO` on
 *         a write that makes no progress, which no system error of its own describes.
 */
static int write_all(int fd, struct Str data);

/**
 * @brief Copies a path into a terminated buffer for a system call.
 *
 * @param path     Path to convert.
 * @param cstr_out Buffer of `PATH_MAX` bytes that receives the terminated path. Must not be `NULL`.
 * @return `0` on success, or `-1` with `errno` set to `ENAMETOOLONG` when the path and its
 *         terminator exceed `PATH_MAX`, or to `EINVAL` when it holds a `NUL` byte.
 */
static int copy_cstr(struct Str path, char cstr_out[static PATH_MAX]);

/**
 * @brief Reports `error_number` as the cause of the caller's operation failing on `path`.
 *
 * This is for a failure on the path the caller's operation targeted, as in
 * `failed to read file: No such file or directory ('src/a.c')`.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param path         Path the operation targeted.
 * @param error_number `errno` value to describe.
 * @return `-1` always, so a caller can write `return report_errno(...);`.
 */
static int report_errno(const struct FsReport* report, struct Str path, int error_number)
    __attribute__((nonnull(1)));

struct Os* os_posix(void) {
  static struct Os os;
  return &os;
}

int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  int fd = -1;
  struct stat st;
  if (open_inspect(file_path, &fd, &st) != 0) {
    return report_errno(&report, file_path, errno);
  }
  if (os_read_check(&st, file_path, data_len_max, &report) != 0) {
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }
  // The size is within `data_len_max`, so it converts to `ptrdiff_t` exactly. Allocating from a
  // copy of `perm` and publishing it only on success leaves `perm` as it found it when the read
  // fails.
  const ptrdiff_t data_len = (ptrdiff_t)st.st_size;
  struct Arena arena = *perm;
  // The read overwrites every byte, so the bytes are not zeroed first: an empty `arena_alloc`
  // places them and `arena_extend` takes them as they are, which saves a pass over every byte read.
  (void)arena_alloc(&arena, 0, 1, 1);
  char* data = arena_extend(&arena, data_len, 1);
  if (os_read_bytes(data, data_len, fd, file_path, &report) != 0) {
    return -1;
  }
  *perm = arena;
  // `os_read_bytes` succeeds only on a full read, so the byte count is the stat size.
  *data_out = (struct Str){data, data_len};
  return 0;
}

int os_read_stdin(struct Os* os,
                  ptrdiff_t data_len_max,
                  struct Arena* perm,
                  struct Str* data_out,
                  struct Str operation,
                  struct Log* log) {
  (void)os;
  assert(data_len_max >= 0);
  const struct FsReport report = {operation, log};
  // Read into a copy of `perm` and publish it only on success, so a failed read leaves `perm` as it
  // found it. Each step extends the buffer in place, because it is the copy's last allocation.
  struct Arena arena = *perm;
  char* data = arena_new(&arena, 0, char);
  ptrdiff_t data_cap = 0;
  ptrdiff_t data_len = 0;
  for (;;) {
    // Ask for at most one byte past the limit, which is enough to tell that the input exceeds it.
    // The loop stops there, so `data_len` never passes the limit and `want` is always positive.
    const ptrdiff_t want = data_len_max - data_len < OS_READ_STDIN_STEP_LEN
                               ? data_len_max - data_len + 1
                               : OS_READ_STDIN_STEP_LEN;
    // A pipe returns what its writer has written so far, often less than `want`, so the buffer
    // grows only when the room left is short of the next read.
    if (data_cap - data_len < want) {
      (void)arena_extend(&arena, OS_READ_STDIN_STEP_LEN, 1);
      data_cap += OS_READ_STDIN_STEP_LEN;
    }
    const ssize_t chunk_len = read(STDIN_FILENO, data + data_len, (size_t)want);
    if (chunk_len < 0 && errno == EINTR) {
      continue;
    }
    if (chunk_len < 0) {
      return os_read_stdin_report_errno(&report, errno);
    }
    if (chunk_len == 0) {
      break;
    }
    if (memchr(data + data_len, '\0', (size_t)chunk_len) != NULL) {
      return log_error(log, "%.*s: contains an embedded NUL byte (from 'stdin')",
                       STR_FMT(operation));
    }
    data_len += chunk_len;
    if (data_len > data_len_max) {
      return log_error(log, "%.*s: exceeds max input size (%td bytes) (from 'stdin')",
                       STR_FMT(operation), data_len_max);
    }
  }
  *perm = arena;
  *data_out = (struct Str){data, data_len};
  return 0;
}

int os_write(struct Os* os,
             struct Str file_path,
             struct Str data,
             struct Str operation,
             struct Log* log) {
  (void)os;
  const struct FsReport report = {operation, log};
  char destination_path[PATH_MAX];
  if (copy_cstr(file_path, destination_path) != 0 || os_write_resolve(destination_path) != 0) {
    return report_errno(&report, file_path, errno);
  }
  char temporary_path[PATH_MAX];
  if (os_write_temporary_path(destination_path, temporary_path) != 0) {
    return report_errno(&report, file_path, errno);
  }

  struct stat st;
  const bool has_existing_file = stat(destination_path, &st) == 0;
  if (!has_existing_file && errno != ENOENT) {
    return report_errno(&report, file_path, errno);
  }

  const mode_t mode = has_existing_file ? st.st_mode & 07777 : os_write_created_mode();

  int rc = -1;
  bool is_renamed = false;

  int fd = mkstemp(temporary_path);
  if (fd < 0) {
    (void)report_errno(&report, file_path, errno);
    goto cleanup;
  }

  rc = 0;
  if (write_all(fd, data) != 0) {
    rc = report_errno(&report, file_path, errno);
  }
  if (rc == 0 && fchmod(fd, mode) != 0) {
    rc = report_errno(&report, file_path, errno);
  }
  // `fsync` before the `rename`, not after. Otherwise the new directory entry can reach the disk
  // while the bytes behind it are still in the page cache, and a power loss leaves the user's
  // source file empty or truncated in place of the previous complete one.
  if (rc == 0 && fsync(fd) != 0) {
    rc = report_errno(&report, file_path, errno);
  }
  if (close(fd) != 0) {
    if (rc == 0) {
      (void)report_errno(&report, file_path, errno);
    }
    rc = -1;
  }
  fd = -1;
  if (rc != 0) {
    goto cleanup;
  }
  if (rename(temporary_path, destination_path) != 0) {
    rc = report_errno(&report, file_path, errno);
    goto cleanup;
  }
  is_renamed = true;

cleanup:
  if (fd >= 0) {
    (void)close(fd);
  }
  if (!is_renamed) {
    (void)unlink(temporary_path);
  }
  return rc;
}

int os_print(struct Os* os, int fd, struct Str text) {
  (void)os;
  return write_all(fd, text);
}

static int open_inspect(struct Str file_path, int* fd_out, struct stat* st_out) {
  char path[PATH_MAX];
  const int fd = copy_cstr(file_path, path) == 0 ? open(path, O_RDONLY | O_NONBLOCK) : -1;
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, st_out) != 0) {
    // Read before `close`, which may change it.
    const int fstat_errno = errno;
    (void)close(fd);
    errno = fstat_errno;
    return -1;
  }
  *fd_out = fd;
  return 0;
}

static int os_read_check(const struct stat* st,
                         struct Str file_path,
                         ptrdiff_t data_len_max,
                         const struct FsReport* report) {
  if (!S_ISREG(st->st_mode)) {
    return log_error(report->log, "%.*s: not a regular file ('%.*s')", STR_FMT(report->operation),
                     STR_FMT(file_path));
  }
  if (st->st_size > data_len_max) {
    // An oversize file costs one `fstat` rather than its whole size in memory. A file that grows
    // past the limit after this check is still caught, because `os_read_bytes_exact` rejects any
    // byte beyond this size.
    return log_error(report->log, "%.*s: exceeds max file size (%td bytes) at %jd bytes ('%.*s')",
                     STR_FMT(report->operation), data_len_max, (intmax_t)st->st_size,
                     STR_FMT(file_path));
  }
  return 0;
}

static int os_read_bytes(char* data,
                         ptrdiff_t data_len,
                         int fd,
                         struct Str file_path,
                         const struct FsReport* report) {
  FILE* fp = fcntl(fd, F_SETFL, 0) == 0 ? fdopen(fd, "rb") : NULL;
  if (fp == NULL) {
    (void)report_errno(report, file_path, errno);
    // The failure above is the one reported, so a close error on this path adds nothing.
    (void)close(fd);
    return -1;
  }
  int rc = os_read_bytes_exact(data, data_len, fp, file_path, report);
  if (fclose(fp) != 0) {
    if (rc == 0) {
      (void)report_errno(report, file_path, errno);
    }
    rc = -1;
  }
  return rc;
}

static int os_read_bytes_exact(char* data,
                               ptrdiff_t data_len,
                               FILE* fp,
                               struct Str file_path,
                               const struct FsReport* report) {
  assert(data_len >= 0);
  // Reset `errno` so a value left by an earlier call cannot pass for the cause of this one. ISO C
  // does not require `fread` to set it, so a stream error that left it at 0 is reported as `EIO`.
  errno = 0;
  const size_t read_len = fread(data, 1, (size_t)data_len, fp);
  // Capture `errno` before calling `ferror`, which is permitted to modify it even when it succeeds.
  // Reading it afterwards could name a cause the read never had.
  const int read_errno = errno;
  int rc = -1;
  if (ferror(fp) != 0) {
    (void)report_errno(report, file_path, read_errno == 0 ? EIO : read_errno);
  } else if (read_len != (size_t)data_len) {
    // No stream error, so the bytes ran out. The file shrank after the caller's `fstat`.
    (void)log_error(report->log, "%.*s: shrank while being read ('%.*s')",
                    STR_FMT(report->operation), STR_FMT(file_path));
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
      (void)report_errno(report, file_path, extra_errno == 0 ? EIO : extra_errno);
    } else if (extra_read == 0) {
      rc = 0;
    } else {
      (void)log_error(report->log, "%.*s: grew while being read ('%.*s')",
                      STR_FMT(report->operation), STR_FMT(file_path));
    }
  }
  // Text is the only thing this program reads, and no format it reads gives a `NUL` a meaning.
  if (rc == 0 && memchr(data, '\0', read_len) != NULL) {
    (void)log_error(report->log, "%.*s: contains an embedded NUL byte ('%.*s')",
                    STR_FMT(report->operation), STR_FMT(file_path));
    rc = -1;
  }
  return rc;
}

static int os_read_stdin_report_errno(const struct FsReport* report, int error_number) {
  return log_error(report->log, "%.*s: %s (from 'stdin')", STR_FMT(report->operation),
                   strerror(error_number));
}

static int os_write_resolve(char path[static PATH_MAX]) {
  struct stat link_st;
  if (lstat(path, &link_st) != 0 || !S_ISLNK(link_st.st_mode)) {
    return 0;
  }
  // `realpath` fails with `ENAMETOOLONG` rather than write past `PATH_MAX` bytes.
  char resolved_path[PATH_MAX];
  if (realpath(path, resolved_path) == NULL) {
    return -1;
  }
  memcpy(path, resolved_path, strlen(resolved_path) + 1);
  return 0;
}

static int os_write_temporary_path(const char destination_path[static 1],
                                   char temporary_out[static PATH_MAX]) {
  static const char suffix[] = ".cwrap.XXXXXX";
  const char* basename = strrchr(destination_path, '/');
  const ptrdiff_t directory_len = basename == NULL ? 0 : basename - destination_path + 1;
  if (directory_len > PATH_MAX - (ptrdiff_t)sizeof(suffix)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(temporary_out, destination_path, (size_t)directory_len);
  memcpy(temporary_out + directory_len, suffix, sizeof(suffix));
  return 0;
}

static mode_t os_write_created_mode(void) {
  const mode_t mask = umask(0);
  (void)umask(mask);
  return (mode_t)(0666 & ~mask);
}

static int write_all(int fd, struct Str data) {
  while (data.len > 0) {
    const ssize_t written_len =
        write(fd, data.data, (size_t)(data.len < FS_WRITE_LEN_MAX ? data.len : FS_WRITE_LEN_MAX));
    if (written_len > 0) {
      data = str_tail(data, written_len);
    } else if (written_len < 0 && errno == EINTR) {
      continue;
    } else if (written_len < 0) {
      return -1;
    } else {
      errno = EIO;
      return -1;
    }
  }
  return 0;
}

static int copy_cstr(struct Str path, char cstr_out[static PATH_MAX]) {
  // The lower bound is the `struct Str` invariant, spelled out so the copy below is visibly in
  // bounds in a build without assertions.
  if (path.len < 0 || path.len >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (path.len > 0 && memchr(path.data, '\0', (size_t)path.len) != NULL) {
    errno = EINVAL;
    return -1;
  }
  // `memcpy` requires valid pointers even for zero bytes, and an empty path may hold `NULL`.
  if (path.len > 0) {
    memcpy(cstr_out, path.data, (size_t)path.len);
  }
  cstr_out[path.len] = '\0';
  return 0;
}

static int report_errno(const struct FsReport* report, struct Str path, int error_number) {
  return log_error(report->log, "%.*s: %s ('%.*s')", STR_FMT(report->operation),
                   strerror(error_number), STR_FMT(path));
}
