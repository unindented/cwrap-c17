// `TEST_NO_MAIN` lets the fixture helpers here record failures with `TEST_CHECK`, as in
// `test_support.c`.
#define TEST_NO_MAIN

// `PATH_MAX` is outside strict C17, so `<limits.h>` hides it unless a feature-test macro asks for
// it, as in `fs.c`: `_DARWIN_C_SOURCE` on macOS, `_DEFAULT_SOURCE` on glibc and musl. Both must
// precede every `#include`.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "os_memory.h"

#include <acutest.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** One path the host holds. */
struct OsMemoryFile {
  /** Exact path text the file is found by. */
  struct Str path;

  /** Whether the path names a directory rather than a regular file. */
  bool is_dir;

  /**
   * A regular file's contents. Never `NULL` for a regular file, so `os_read` can pass it to
   * `memchr` when the file is empty.
   */
  struct Str data;

  /**
   * Bytes past `data` that a regular file holds and that read as `NUL`, from `os_memory_truncate`.
   * A write replaces them along with `data`.
   */
  ptrdiff_t zero_len;
};

/** One injected failure. */
struct OsMemoryFault {
  /** Operation that fails. */
  enum OsMemoryOp op;

  /** Exact path the failure applies to. */
  struct Str path;

  /** `errno` value the operation fails with. */
  int error_number;
};

/** One standard output stream as `os_print` sees it. */
struct OsMemoryStream {
  /** Text printed so far. */
  struct Buf printed;

  /** Bytes the stream accepts before it fails, or `-1` for a stream that never fails. */
  ptrdiff_t fail_len;

  /** `errno` value a print past `fail_len` fails with. */
  int fail_errno;
};

/** The in-memory host. */
struct Os {
  /** Arena that owns the files and the printed text. Nothing is freed. */
  struct Arena arena;

  /** Array of `file_capacity` files. */
  struct OsMemoryFile* files;

  /** Number of populated entries in `files`. */
  ptrdiff_t file_count;

  /** Allocated slots in `files`. */
  ptrdiff_t file_capacity;

  /** Array of `fault_capacity` injected failures. */
  struct OsMemoryFault* faults;

  /** Number of populated entries in `faults`. */
  ptrdiff_t fault_count;

  /** Allocated slots in `faults`. */
  ptrdiff_t fault_capacity;

  /** Bytes standard input holds. */
  struct Str stdin_data;

  /** `0` for an end of input after `stdin_data`, or the `errno` value a read past it fails with. */
  int stdin_errno;

  /** Standard output, then standard error. */
  struct OsMemoryStream streams[2];
};

/**
 * Where an operation reports its failure, as in the POSIX implementation: the caller's log and
 * operation.
 */
struct OsMemoryReport {
  /** Operation the caller was performing. */
  struct Str operation;

  /** Log that receives the message. */
  struct Log* log;
};

/**
 * @brief Returns the file at exactly `path`, or `NULL`.
 *
 * @param os   Host to look in. Must not be `NULL`.
 * @param path Path to look up.
 * @return The file, or `NULL` when the host holds nothing at `path`.
 */
static struct OsMemoryFile* file_lookup(struct Os* os, struct Str path) __attribute__((nonnull(1)));

/**
 * @brief Returns the file at exactly `path`, adding an empty regular file there when there is none.
 *
 * @param os   Host to add to. Must not be `NULL`.
 * @param path Path of the file.
 * @return The file, never `NULL`.
 */
static struct OsMemoryFile* file_add(struct Os* os, struct Str path)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Returns the `errno` value an injected failure gives `op` on `path`, or `0` for none.
 *
 * @param os   Host to look in. Must not be `NULL`.
 * @param op   Operation the caller performs.
 * @param path Path the caller passed.
 * @return The injected `errno` value, or `0`.
 */
static int fault_find(const struct Os* os, enum OsMemoryOp op, struct Str path)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the `errno` value the POSIX host would fail a path with before any system call,
 *        when its `copy_cstr` refuses the path.
 *
 * @param path Path the caller passed.
 * @return `ENAMETOOLONG` for a path of `PATH_MAX` bytes or more, `EINVAL` for one holding a `NUL`
 *         byte, or `0` for a path the system could take whole.
 */
static int cstr_errno(struct Str path);

/**
 * @brief Returns the stream `os_print` writes `fd` to.
 *
 * @param os Host holding the streams. Must not be `NULL`.
 * @param fd `1` for standard output or `2` for standard error.
 * @return The stream, never `NULL`.
 */
static struct OsMemoryStream* stream_of(struct Os* os, int fd)
    __attribute__((nonnull(1), returns_nonnull));

/**
 * @brief Reports `error_number` as the cause of the caller's operation failing on `path`, in the
 *        words of the POSIX implementation.
 *
 * @param report       Where the failure is reported. Must not be `NULL`.
 * @param path         Path the operation targeted.
 * @param error_number `errno` value to describe.
 * @return `-1` always.
 */
static int report_errno(const struct OsMemoryReport* report, struct Str path, int error_number)
    __attribute__((nonnull(1)));

struct Os* os_memory(void) {
  struct Arena arena = test_arena();
  struct Os* os = arena_new(&arena, 1, struct Os);
  os->arena = arena;
  for (ptrdiff_t i = 0; i < 2; i++) {
    os->streams[i] = (struct OsMemoryStream){.printed = {.arena = &os->arena}, .fail_len = -1};
  }
  return os;
}

void os_memory_write(struct Os* os, const char* file_path, const char* contents) {
  os_memory_write_bytes(os, str_from_cstr(file_path), str_from_cstr(contents));
}

void os_memory_write_bytes(struct Os* os, struct Str file_path, struct Str contents) {
  struct OsMemoryFile* file = file_add(os, file_path);
  file->is_dir = false;
  file->data = str_clone(contents, &os->arena);
  file->zero_len = 0;
}

void os_memory_truncate(struct Os* os, const char* file_path, ptrdiff_t len) {
  struct OsMemoryFile* file = file_lookup(os, str_from_cstr(file_path));
  if (!TEST_CHECK_(file != NULL && !file->is_dir, "'%s' is a regular file", file_path)) {
    return;
  }
  assert(len >= file->data.len + file->zero_len);
  file->zero_len = len - file->data.len;
}

void os_memory_mkdir(struct Os* os, const char* dir_path) {
  struct OsMemoryFile* file = file_add(os, str_from_cstr(dir_path));
  *file = (struct OsMemoryFile){.path = file->path, .is_dir = true};
}

void os_memory_stdin(struct Os* os, struct Str contents, int error_number) {
  os->stdin_data = str_clone(contents, &os->arena);
  os->stdin_errno = error_number;
}

void os_memory_fail(struct Os* os, enum OsMemoryOp op, const char* path, int error_number) {
  if (os->fault_count == os->fault_capacity) {
    os->faults = arena_grow(&os->arena, os->faults, &os->fault_capacity, sizeof(*os->faults),
                            _Alignof(struct OsMemoryFault));
  }
  os->faults[os->fault_count++] = (struct OsMemoryFault){
      .op = op, .path = str_clone(str_from_cstr(path), &os->arena), .error_number = error_number};
}

void os_memory_fail_print(struct Os* os, int fd, ptrdiff_t len, int error_number) {
  assert(len >= 0);
  struct OsMemoryStream* stream = stream_of(os, fd);
  stream->fail_len = len;
  stream->fail_errno = error_number;
}

bool os_memory_is_file(struct Os* os, const char* file_path, const char* contents) {
  const struct OsMemoryFile* file = file_lookup(os, str_from_cstr(file_path));
  const bool is_file = file != NULL && !file->is_dir;
  const ptrdiff_t zero_len = is_file ? file->zero_len : 0;
  const struct Str data = is_file ? file->data : (struct Str){0};
  // A file extended by `os_memory_truncate` holds bytes `contents` cannot spell, so it never
  // matches.
  const bool is_equal = is_file && zero_len == 0 && str_eq(data, str_from_cstr(contents));
  TEST_CHECK_(is_equal, "'%s' holds the expected text", file_path);
  if (!is_file) {
    TEST_MSG("no regular file at '%s'", file_path);
  } else if (zero_len > 0) {
    TEST_MSG("'%s' holds %td bytes past its contents", file_path, zero_len);
  } else {
    TEST_MSG("expected: '%s'", contents);
    TEST_MSG("actual:   '%.*s'", STR_FMT(data));
  }
  return is_equal;
}

struct Str os_memory_printed(struct Os* os, int fd) {
  return buf_str(&stream_of(os, fd)->printed);
}

bool os_memory_is_printed(struct Os* os, int fd, const char* expected) {
  const struct Str text = os_memory_printed(os, fd);
  const bool is_equal = str_eq(text, str_from_cstr(expected));
  TEST_CHECK_(is_equal, "standard %s holds the expected text", fd == 1 ? "output" : "error");
  TEST_MSG("expected: '%s'", expected);
  TEST_MSG("actual:   '%.*s'", STR_FMT(text));
  return is_equal;
}

int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  const int path_errno = cstr_errno(file_path);
  if (path_errno != 0) {
    return report_errno(&report, file_path, path_errno);
  }
  const int open_errno = fault_find(os, OS_MEMORY_OP_OPEN, file_path);
  if (open_errno != 0) {
    return report_errno(&report, file_path, open_errno);
  }
  const struct OsMemoryFile* file = file_lookup(os, file_path);
  if (file == NULL) {
    return report_errno(&report, file_path, ENOENT);
  }
  if (file->is_dir) {
    return log_error(log, "%.*s: not a regular file ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  const ptrdiff_t len = file->data.len + file->zero_len;
  if (len > data_len_max) {
    return log_error(log, "%.*s: exceeds max file size (%td bytes) at %td bytes ('%.*s')",
                     STR_FMT(operation), data_len_max, len, STR_FMT(file_path));
  }
  const int read_errno = fault_find(os, OS_MEMORY_OP_READ, file_path);
  if (read_errno != 0) {
    return report_errno(&report, file_path, read_errno);
  }
  if (file->zero_len > 0 || memchr(file->data.data, '\0', (size_t)file->data.len) != NULL) {
    return log_error(log, "%.*s: contains an embedded NUL byte ('%.*s')", STR_FMT(operation),
                     STR_FMT(file_path));
  }
  *data_out = str_clone(file->data, perm);
  return 0;
}

int os_read_stdin(struct Os* os,
                  ptrdiff_t data_len_max,
                  struct Arena* perm,
                  struct Str* data_out,
                  struct Str operation,
                  struct Log* log) {
  assert(data_len_max >= 0);
  // The POSIX host reads at most one byte past the limit and checks each piece for a `NUL` before
  // it counts the piece against the limit, so a `NUL` among those bytes wins over the limit.
  const struct Str data = os->stdin_data;
  const struct Str seen = str_head(data, data.len <= data_len_max ? data.len : data_len_max + 1);
  if (seen.len > 0 && memchr(seen.data, '\0', (size_t)seen.len) != NULL) {
    return log_error(log, "%.*s: contains an embedded NUL byte (from 'stdin')", STR_FMT(operation));
  }
  if (data.len > data_len_max) {
    return log_error(log, "%.*s: exceeds max input size (%td bytes) (from 'stdin')",
                     STR_FMT(operation), data_len_max);
  }
  if (os->stdin_errno != 0) {
    return log_error(log, "%.*s: %s (from 'stdin')", STR_FMT(operation), strerror(os->stdin_errno));
  }
  *data_out = str_clone(data, perm);
  return 0;
}

int os_write(struct Os* os,
             struct Str file_path,
             struct Str data,
             struct Str operation,
             struct Log* log) {
  const struct OsMemoryReport report = {operation, log};
  const int path_errno = cstr_errno(file_path);
  if (path_errno != 0) {
    return report_errno(&report, file_path, path_errno);
  }
  const int write_errno = fault_find(os, OS_MEMORY_OP_WRITE, file_path);
  if (write_errno != 0) {
    return report_errno(&report, file_path, write_errno);
  }
  const struct OsMemoryFile* existing = file_lookup(os, file_path);
  if (existing != NULL && existing->is_dir) {
    // The POSIX host renames its temporary over the directory, which `rename` refuses.
    return report_errno(&report, file_path, EISDIR);
  }
  os_memory_write_bytes(os, file_path, data);
  return 0;
}

int os_print(struct Os* os, int fd, struct Str text) {
  struct OsMemoryStream* stream = stream_of(os, fd);
  const ptrdiff_t printed_len = stream->printed.len;
  if (stream->fail_len >= 0 && printed_len + text.len > stream->fail_len) {
    const ptrdiff_t room = printed_len < stream->fail_len ? stream->fail_len - printed_len : 0;
    buf_append(&stream->printed, str_head(text, room));
    errno = stream->fail_errno;
    return -1;
  }
  buf_append(&stream->printed, text);
  return 0;
}

static struct OsMemoryFile* file_lookup(struct Os* os, struct Str path) {
  for (ptrdiff_t i = 0; i < os->file_count; i++) {
    if (str_eq(os->files[i].path, path)) {
      return &os->files[i];
    }
  }
  return NULL;
}

static struct OsMemoryFile* file_add(struct Os* os, struct Str path) {
  struct OsMemoryFile* file = file_lookup(os, path);
  if (file != NULL) {
    return file;
  }
  if (os->file_count == os->file_capacity) {
    os->files = arena_grow(&os->arena, os->files, &os->file_capacity, sizeof(*os->files),
                           _Alignof(struct OsMemoryFile));
  }
  file = &os->files[os->file_count++];
  *file = (struct OsMemoryFile){.path = str_clone(path, &os->arena), .data = S("")};
  return file;
}

static int fault_find(const struct Os* os, enum OsMemoryOp op, struct Str path) {
  for (ptrdiff_t i = 0; i < os->fault_count; i++) {
    const struct OsMemoryFault* fault = &os->faults[i];
    if (fault->op == op && str_eq(path, fault->path)) {
      return fault->error_number;
    }
  }
  return 0;
}

static int cstr_errno(struct Str path) {
  if (path.len >= PATH_MAX) {
    return ENAMETOOLONG;
  }
  if (path.len > 0 && memchr(path.data, '\0', (size_t)path.len) != NULL) {
    return EINVAL;
  }
  return 0;
}

static struct OsMemoryStream* stream_of(struct Os* os, int fd) {
  assert(fd == 1 || fd == 2);
  return &os->streams[fd == 1 ? 0 : 1];
}

static int report_errno(const struct OsMemoryReport* report, struct Str path, int error_number) {
  return log_error(report->log, "%.*s: %s ('%.*s')", STR_FMT(report->operation),
                   strerror(error_number), STR_FMT(path));
}
