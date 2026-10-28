// `truncate`, `fork` and `setrlimit` are POSIX rather than C17, so the POSIX build of this file
// asks for them.
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

#if CWRAP_TEST_OS_MEMORY
#include "os_memory.h"
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "runtime/fs.h"
#endif

// These are the contract tests of `runtime/os.h`. This one file builds two executables: one links
// the POSIX host in `fs.c` and runs against a real temporary directory and the process's own
// standard streams, and one links the in-memory host in `tests/os_memory.c`. Both must pass. Every
// property that a test of another module checks against the in-memory host is checked here against
// both, so the in-memory host cannot accept what POSIX rejects. Facts only a real filesystem has,
// such as FIFOs, modes, permission bits and symbolic links, are in `test_fs.c`, which also
// produces, from permission bits, a failure the in-memory host can only inject: a file the process
// may not open.
//
// Every call under test here runs with the operation `op`, so an expected failure reads
// `op: <cause>`. A system cause is spelled with `strerror` from the running libc, as both hosts
// spell it.

/** Read limit, in bytes, for the small fixture files most tests here read back. */
enum { TEST_FILE_LEN_MAX = 1024 };

/** Bytes of each standard stream `test_print_writes_each_stream_in_order` reads back. */
enum { TEST_PRINTED_SIZE = 64 };

/** Bytes standard output accepts in `test_print_keeps_bytes_before_failure` before it fails. */
enum { TEST_PRINT_LEN_MAX = 4 };

/** One test's host and the directory its files live in. */
struct Contract {
  /** Host under test. */
  struct Os* os;

  /** Terminated path of the test's directory: a new temporary directory, or a fixed one. */
  const char* root_dir;

  /** Arena for paths and read data. */
  struct Arena arena;

  /** Duplicate of the process's standard input while `contract_stdin` replaced it, or `-1`. */
  int stdin_fd_saved;

  /** Write end of the pipe `contract_stdin` made standard input, held open, or `-1`. */
  int stdin_writer_fd;
};

/**
 * @brief Starts a test: sets up the host and an empty directory for its files.
 *
 * @param root_template Writable `mkdtemp` template. The POSIX host creates the directory there. The
 *                      in-memory host uses the template as it stands. Must not be `NULL`.
 * @param contract_out  Receives the contract. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure when the directory could
 *         not be created.
 */
static int contract_begin(char root_template[static 1], struct Contract* contract_out) {
  *contract_out = (struct Contract){.root_dir = root_template,
                                    .arena = test_arena(),
                                    .stdin_fd_saved = -1,
                                    .stdin_writer_fd = -1};
#if CWRAP_TEST_OS_MEMORY
  contract_out->os = os_memory();
  os_memory_mkdir(contract_out->os, root_template);
  return 0;
#else
  contract_out->os = os_posix();
  return init_fixture_dir(root_template);
#endif
}

/**
 * @brief Ends a test: restores the POSIX host's standard input and removes its directory.
 *
 * @param contract Contract that `contract_begin` set up. Must not be `NULL`.
 */
static void contract_end(const struct Contract* contract) {
#if CWRAP_TEST_OS_MEMORY
  (void)contract;
#else
  if (contract->stdin_fd_saved >= 0) {
    TEST_CHECK(dup2(contract->stdin_fd_saved, STDIN_FILENO) == STDIN_FILENO);
    TEST_CHECK(close(contract->stdin_fd_saved) == 0);
  }
  if (contract->stdin_writer_fd >= 0) {
    TEST_CHECK(close(contract->stdin_writer_fd) == 0);
  }
  remove_fixture_tree(contract->root_dir);
#endif
}

/**
 * @brief Returns `relative_path` below the test's directory, with a terminator after it.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str contract_path(struct Contract* contract, const char* relative_path) {
  struct Buf path = {.arena = &contract->arena};
  buf_printf(&path, "%s/%s", contract->root_dir, relative_path);
  return (struct Str){buf_cstr(&path), path.len};
}

/**
 * @brief Writes a fixture file below the test's directory.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @param data          Bytes to write.
 */
static void contract_write(struct Contract* contract, const char* relative_path, struct Str data) {
  struct Log log = test_log();
  TEST_CHECK(os_write(contract->os, contract_path(contract, relative_path), data,
                      S("failed to write fixture"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

/**
 * @brief Extends a file below the test's directory to `len` bytes, as `truncate` does.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path of an existing file below the directory. Must not be `NULL`.
 * @param len           New size in bytes, at least the file's size.
 */
static void contract_truncate(struct Contract* contract, const char* relative_path, ptrdiff_t len) {
  const struct Str file_path = contract_path(contract, relative_path);
#if CWRAP_TEST_OS_MEMORY
  os_memory_truncate(contract->os, file_path.data, len);
#else
  TEST_CHECK(truncate(file_path.data, (off_t)len) == 0);
#endif
}

/**
 * @brief Gives the host's standard input `contents`, and how a read past them ends.
 *
 * Until `contract_end` restores it, the POSIX host's standard input becomes a file holding
 * `contents` below the test's directory for an end of input, a descriptor open only for writing for
 * `EBADF`, or a non-blocking pipe holding `contents`, whose writer stays open, for `EAGAIN`.
 *
 * @param contract     Contract of the test. Must not be `NULL`.
 * @param contents     Bytes standard input holds. Empty for `EBADF`, and no more than a pipe holds
 *                     for `EAGAIN`.
 * @param error_number `0` for an end of input after `contents`, `EBADF` for a read that fails
 *                     before any byte, or `EAGAIN` for a read past `contents` that fails.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int contract_stdin(struct Contract* contract, struct Str contents, int error_number) {
#if CWRAP_TEST_OS_MEMORY
  os_memory_stdin(contract->os, contents, error_number);
  return 0;
#else
  int fd = -1;
  if (error_number == EBADF) {
    fd = open("/dev/null", O_WRONLY);
  } else if (error_number == EAGAIN) {
    int pipe_fds[2];
    if (!TEST_CHECK(pipe(pipe_fds) == 0)) {
      return -1;
    }
    fd = pipe_fds[0];
    contract->stdin_writer_fd = pipe_fds[1];
    TEST_CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    TEST_CHECK(write(pipe_fds[1], contents.data, (size_t)contents.len) == contents.len);
  } else {
    contract_write(contract, "stdin", contents);
    fd = open(contract_path(contract, "stdin").data, O_RDONLY);
  }
  if (!TEST_CHECK(fd >= 0)) {
    return -1;
  }
  contract->stdin_fd_saved = dup(STDIN_FILENO);
  const bool is_redirected = TEST_CHECK(contract->stdin_fd_saved >= 0) &&
                             TEST_CHECK(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
  TEST_CHECK(close(fd) == 0);
  return is_redirected ? 0 : -1;
#endif
}

/**
 * @brief Reads a file below the test's directory, and reports whether it holds `expected`.
 *
 * @param contract      Contract of the test. Must not be `NULL`.
 * @param relative_path Terminated path below the directory. Must not be `NULL`.
 * @param expected      Bytes the file must hold.
 * @return `true` when the read succeeds with exactly `expected`.
 */
static bool contract_is_file(struct Contract* contract,
                             const char* relative_path,
                             struct Str expected) {
  struct Log log = test_log();
  struct Str data = {0};
  const bool is_read = os_read(contract->os, contract_path(contract, relative_path),
                               TEST_FILE_LEN_MAX, &contract->arena, &data, S("op"), &log) == 0;
  const struct Str failure = buf_str(&log.buf);
  TEST_MSG("%.*s", STR_FMT(failure));
  return is_read && str_eq(data, expected);
}

/**
 * @brief Prints `ab`, `cdef` and then `g` on standard output, which accepts `TEST_PRINT_LEN_MAX`
 *        bytes, and reports whether the first print succeeds and the other two fail with `EFBIG`.
 *
 * The second print crosses the limit and the third starts past it.
 *
 * @param os Host to print on. Must not be `NULL`.
 * @return `true` when the three prints end as described.
 */
static bool is_print_cut_at_limit(struct Os* os) {
  const int print_ab_rc = os_print(os, 1, S("ab"));
  errno = 0;
  const int print_cdef_rc = os_print(os, 1, S("cdef"));
  const int cdef_errno = errno;
  errno = 0;
  const int print_g_rc = os_print(os, 1, S("g"));
  return print_ab_rc == 0 && print_cdef_rc == -1 && cdef_errno == EFBIG && print_g_rc == -1 &&
         errno == EFBIG;
}

#if !CWRAP_TEST_OS_MEMORY
/**
 * @brief Overwrites the file at `file_path` under a file size limit below its new size, and exits
 *        with `0` when the write failed. Run in a child process, which the limit would otherwise
 *        outlive.
 *
 * @param file_path Path of the file.
 * @param data      Bytes to write, more than the limit allows.
 */
static _Noreturn void write_past_size_limit(struct Str file_path, struct Str data) {
  const struct rlimit limit = {.rlim_cur = 1024, .rlim_max = 1024};
  (void)signal(SIGXFSZ, SIG_IGN);
  if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
    _exit(2);
  }
  struct Log log = test_log();
  _exit(os_write(os_posix(), file_path, data, S("op"), &log) == -1 ? 0 : 3);
}

/**
 * @brief Points standard output at a new file at `file_path` under a file size limit of
 *        `TEST_PRINT_LEN_MAX` bytes, and exits with `0` when `is_print_cut_at_limit` reports what
 *        it should. Run in a child process, which the limit and the redirection would otherwise
 *        outlive.
 *
 * @param file_path Terminated path of the file. Must not be `NULL`.
 */
static _Noreturn void print_past_size_limit(const char* file_path) {
  const struct rlimit limit = {.rlim_cur = TEST_PRINT_LEN_MAX, .rlim_max = TEST_PRINT_LEN_MAX};
  (void)signal(SIGXFSZ, SIG_IGN);
  const int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0 || dup2(fd, STDOUT_FILENO) != STDOUT_FILENO || setrlimit(RLIMIT_FSIZE, &limit) != 0) {
    _exit(2);
  }
  _exit(is_print_cut_at_limit(os_posix()) ? 0 : 3);
}
#endif

// An empty file reads back as zero bytes, and a file exactly at `data_len_max` bytes reads in full,
// so the limit is inclusive.
static void test_read_accepts_empty_and_file_at_limit(void) {
  char root[] = "/tmp/cwrap-os-read-limit.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  contract_write(&c, "empty.c", S(""));
  contract_write(&c, "four.c", S("four"));

  struct Str data = S("unchanged");
  TEST_CHECK(os_read(c.os, contract_path(&c, "empty.c"), TEST_FILE_LEN_MAX, &c.arena, &data,
                     S("op"), &log) == 0);
  TEST_CHECK(data.len == 0);
  TEST_CHECK(os_read(c.os, contract_path(&c, "four.c"), S("four").len, &c.arena, &data, S("op"),
                     &log) == 0);
  TEST_CHECK(str_eq(data, S("four")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A file one byte over `data_len_max` is rejected with the limit and the size, and the output is
// left as it was.
static void test_read_rejects_oversize(void) {
  char root[] = "/tmp/cwrap-os-read-oversize.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  contract_write(&c, "five.c", S("five!"));

  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  const struct Str path = contract_path(&c, "five.c");
  TEST_CHECK(os_read(c.os, path, 4, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: exceeds max file size (4 bytes) at 5 bytes ('%.*s')\n",
                         STR_FMT(path)));
  contract_end(&c);
}

// A file extended past its bytes has its whole size checked against the limit before any byte is
// read. The file is sparse on disk, so the size costs nothing. Within the limit, the bytes past the
// old end read as `NUL` bytes, which the read rejects.
static void test_read_checks_size_of_extended_file(void) {
  enum { BIG_LEN = 1024 * 1024 * 1024 };
  char root[] = "/tmp/cwrap-os-read-extended.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "big.c", S("a"));
  contract_truncate(&c, "big.c", BIG_LEN);
  contract_write(&c, "small.c", S("a"));
  contract_truncate(&c, "small.c", 4);

  const struct Str big = contract_path(&c, "big.c");
  struct Str data = {0};
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, big, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: exceeds max file size (%d bytes) at %d bytes ('%.*s')\n",
                         TEST_FILE_LEN_MAX, BIG_LEN, STR_FMT(big)));

  const struct Str small = contract_path(&c, "small.c");
  log = test_log();
  TEST_CHECK(os_read(c.os, small, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: contains an embedded NUL byte ('%.*s')\n", STR_FMT(small)));
  contract_end(&c);
}

// A missing path fails with the system's message, and a directory is not a regular file, which is a
// first-party message.
static void test_read_rejects_missing_and_directory(void) {
  char root[] = "/tmp/cwrap-os-read-missing.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str missing = contract_path(&c, "missing.c");
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, missing, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOENT), STR_FMT(missing)));

  log = test_log();
  TEST_CHECK(os_read(c.os, str_from_cstr(c.root_dir), TEST_FILE_LEN_MAX, &c.arena, &data, S("op"),
                     &log) == -1);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%s')\n", c.root_dir));
  contract_end(&c);
}

// A file holding a `NUL` byte is rejected after it is read, and `perm` ends where it started.
static void test_read_rejects_embedded_nul(void) {
  char root[] = "/tmp/cwrap-os-read-nul.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "nul.c", S("before\0after"));
  const struct Str path = contract_path(&c, "nul.c");

  struct Log log = test_log();
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  char* const perm_beg = c.arena.beg;
  TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(c.arena.beg == perm_beg);
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "op: contains an embedded NUL byte ('%.*s')\n", STR_FMT(path)));
  contract_end(&c);
}

// A path holding a `NUL` byte fails as `EINVAL`, and a path of `PATH_MAX` bytes fails as
// `ENAMETOOLONG`, even when the bytes before the `NUL` name a real file.
static void test_read_rejects_path_with_nul_and_path_at_path_max(void) {
  char root[] = "/tmp/cwrap-os-read-path.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "a.c", S("a"));
  const struct Str nul_path = str_concat(contract_path(&c, "a.c"), S("\0b"), &c.arena);

  struct Str data = {0};
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, nul_path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  // `%.*s` stops at the first `NUL`, so the message quotes the path only up to it.
  const struct Str nul_path_head = contract_path(&c, "a.c");
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EINVAL), STR_FMT(nul_path_head)));

  static char long_path[PATH_MAX];
  long_path[0] = '/';
  for (ptrdiff_t i = 1; i < (ptrdiff_t)sizeof(long_path); i++) {
    long_path[i] = i % 64 == 0 ? '/' : 'a';
  }
  const struct Str long_path_str = {long_path, PATH_MAX};
  log = test_log();
  TEST_CHECK(os_read(c.os, long_path_str, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENAMETOOLONG), STR_FMT(long_path_str)));
  contract_end(&c);
}

// A path that ends in `/` must name a directory. A file, or on the POSIX host a symlink to one,
// fails as `ENOTDIR` in a read and in a write, which leaves the file as it was. A directory
// followed by `/` is not a regular file to read, and a write over it fails as `EISDIR`.
static void test_trailing_slash_requires_directory(void) {
  char root[] = "/tmp/cwrap-os-trailing-slash.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  contract_write(&c, "file", S("x"));
  static const char* const not_dirs[] = {
      "file/",
#if !CWRAP_TEST_OS_MEMORY
      "file-link/",
#endif
  };
#if !CWRAP_TEST_OS_MEMORY
  TEST_CHECK(symlink("file", contract_path(&c, "file-link").data) == 0);
#endif
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(not_dirs) / sizeof(not_dirs[0])); i++) {
    TEST_CASE(not_dirs[i]);
    const struct Str path = contract_path(&c, not_dirs[i]);
    struct Str data = {0};
    struct Log log = test_log();
    TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOTDIR), STR_FMT(path)));

    log = test_log();
    TEST_CHECK(os_write(c.os, path, S("y"), S("op"), &log) == -1);
    TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOTDIR), STR_FMT(path)));
    TEST_CHECK(contract_is_file(&c, "file", S("x")));
  }
  TEST_CASE_(NULL);

  const struct Str dir = contract_path(&c, "");
  struct Str data = {0};
  struct Log log = test_log();
  TEST_CHECK(os_read(c.os, dir, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: not a regular file ('%.*s')\n", STR_FMT(dir)));
  log = test_log();
  TEST_CHECK(os_write(c.os, dir, S("y"), S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EISDIR), STR_FMT(dir)));
  contract_end(&c);
}

// Standard input reads to its end without being closed, records nothing, and accumulates input
// larger than one read step whole. The bytes start where the next allocation from `perm` would, and
// its free space then begins past them.
static void test_read_stdin_returns_every_byte(void) {
  enum { SOURCE_LEN = 200000 };
  char root[] = "/tmp/cwrap-os-stdin.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  char* source = arena_new(&c.arena, SOURCE_LEN, char);
  memset(source, 'x', SOURCE_LEN);
  struct Arena probe = {0};
  char* perm_beg = NULL;
  struct Str data = {0};
  struct Log log = test_log();
  if (contract_stdin(&c, (struct Str){source, SOURCE_LEN}, 0) != 0) {
    goto cleanup;
  }

  probe = c.arena;
  perm_beg = arena_new(&probe, 0, char);
  TEST_CHECK(os_read_stdin(c.os, TEST_FILE_LEN_MAX * TEST_FILE_LEN_MAX, &c.arena, &data, S("op"),
                           &log) == 0);
  TEST_CHECK(data.data == perm_beg);
  TEST_CHECK(c.arena.beg >= data.data + data.len);
  TEST_CHECK(str_eq(data, (struct Str){source, SOURCE_LEN}));
  TEST_CHECK(test_log_is_empty(&log));

cleanup:
  contract_end(&c);
}

// Empty standard input reads as zero bytes, and input exactly at the limit reads in full, so the
// limit is inclusive.
static void test_read_stdin_accepts_empty_and_input_at_limit(void) {
  char empty_root[] = "/tmp/cwrap-os-stdin-empty.XXXXXX";
  struct Contract c;
  if (contract_begin(empty_root, &c) != 0) {
    return;
  }
  struct Str data = S("unchanged");
  struct Log log = test_log();
  if (contract_stdin(&c, S(""), 0) == 0) {
    TEST_CHECK(os_read_stdin(c.os, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == 0);
    TEST_CHECK(data.len == 0);
  }
  contract_end(&c);

  char limit_root[] = "/tmp/cwrap-os-stdin-limit.XXXXXX";
  if (contract_begin(limit_root, &c) != 0) {
    return;
  }
  if (contract_stdin(&c, S("four"), 0) == 0) {
    TEST_CHECK(os_read_stdin(c.os, S("four").len, &c.arena, &data, S("op"), &log) == 0);
    TEST_CHECK(str_eq(data, S("four")));
  }
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// Standard input holding a `NUL` byte, and input one byte over the limit, are rejected naming the
// stream, and leave the output and `perm` untouched.
static void test_read_stdin_rejects_embedded_nul_and_input_over_limit(void) {
  char nul_root[] = "/tmp/cwrap-os-stdin-nul.XXXXXX";
  struct Contract c;
  if (contract_begin(nul_root, &c) != 0) {
    return;
  }
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  struct Log log = test_log();
  char* perm_beg = c.arena.beg;
  if (contract_stdin(&c, S("a\0b"), 0) == 0) {
    perm_beg = c.arena.beg;
    TEST_CHECK(os_read_stdin(c.os, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(c.arena.beg == perm_beg);
    TEST_CHECK(test_log_is(&log, "op: contains an embedded NUL byte (from 'stdin')\n"));
  }
  contract_end(&c);

  char limit_root[] = "/tmp/cwrap-os-stdin-over.XXXXXX";
  if (contract_begin(limit_root, &c) != 0) {
    return;
  }
  log = test_log();
  if (contract_stdin(&c, S("five!"), 0) == 0) {
    perm_beg = c.arena.beg;
    TEST_CHECK(os_read_stdin(c.os, 4, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(c.arena.beg == perm_beg);
    TEST_CHECK(test_log_is(&log, "op: exceeds max input size (4 bytes) (from 'stdin')\n"));
  }
  TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
  contract_end(&c);
}

// A read error reports the system's message, naming the stream, and leaves the output untouched. A
// standard input open only for writing is the deterministic way there: every read fails with
// `EBADF`.
static void test_read_stdin_reports_read_error(void) {
  char root[] = "/tmp/cwrap-os-stdin-error.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  struct Log log = test_log();
  if (contract_stdin(&c, S(""), EBADF) == 0) {
    TEST_CHECK(os_read_stdin(c.os, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
    TEST_CHECK(test_log_is(&log, "op: %s (from 'stdin')\n", strerror(EBADF)));
  }
  contract_end(&c);
}

// A read error after some input fails the whole read: the bytes before it are not handed back, and
// `perm` ends where it started. On the POSIX host standard input is a non-blocking pipe holding the
// bytes, whose writer stays open, so the read past them fails with `EAGAIN`, and the in-memory host
// is told to fail it the same way.
static void test_read_stdin_reports_error_after_data(void) {
  char root[] = "/tmp/cwrap-os-stdin-error-after.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str sentinel = S("unchanged");
  struct Str data = sentinel;
  struct Log log = test_log();
  if (contract_stdin(&c, S("// one\n"), EAGAIN) == 0) {
    char* const perm_beg = c.arena.beg;
    TEST_CHECK(os_read_stdin(c.os, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == -1);
    TEST_CHECK(c.arena.beg == perm_beg);
    TEST_CHECK(data.data == sentinel.data && data.len == sentinel.len);
    TEST_CHECK(test_log_is(&log, "op: %s (from 'stdin')\n", strerror(EAGAIN)));
  }
  contract_end(&c);
}

// A write creates a file that reads back byte for byte. The bytes are the first allocation in
// `perm`'s free space, which then begins right past them. A second write replaces the contents,
// including bytes past the new end.
static void test_write_round_trips_and_replaces(void) {
  char root[] = "/tmp/cwrap-os-write.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  const struct Str path = contract_path(&c, "a.c");
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, path, S("root"), S("op"), &log) == 0);

  struct Arena probe = c.arena;
  char* const perm_beg = arena_new(&probe, 0, char);
  struct Str data = {0};
  TEST_CHECK(os_read(c.os, path, TEST_FILE_LEN_MAX, &c.arena, &data, S("op"), &log) == 0);
  TEST_CHECK(data.data == perm_beg);
  TEST_CHECK(c.arena.beg == perm_beg + S("root").len);
  TEST_CHECK(str_eq(data, S("root")));

  TEST_CHECK(os_write(c.os, path, S("ab"), S("op"), &log) == 0);
  TEST_CHECK(contract_is_file(&c, "a.c", S("ab")));
  TEST_CHECK(test_log_is_empty(&log));
  contract_end(&c);
}

// A write fails on a directory with the system's message, and on a path holding a `NUL` byte as
// `EINVAL`, quoting the path up to that byte and leaving the file the bytes before it name alone.
static void test_write_rejects_directory_and_path_with_nul(void) {
  char root[] = "/tmp/cwrap-os-write-reject.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, str_from_cstr(c.root_dir), S("x"), S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%s')\n", strerror(EISDIR), c.root_dir));

  contract_write(&c, "a.c", S("a"));
  const struct Str nul_path = str_concat(contract_path(&c, "a.c"), S("\0b"), &c.arena);
  log = test_log();
  TEST_CHECK(os_write(c.os, nul_path, S("b"), S("op"), &log) == -1);
  const struct Str nul_path_head = contract_path(&c, "a.c");
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(EINVAL), STR_FMT(nul_path_head)));
  TEST_CHECK(contract_is_file(&c, "a.c", S("a")));
  contract_end(&c);
}

// A write that fails partway leaves the file it would replace byte for byte as it was. On the POSIX
// host a file size limit stops the write. The in-memory host is told to fail it with `ENOSPC`.
static void test_write_failure_leaves_old_file(void) {
  enum { CONTENTS_LEN = 2048 };
  char root[] = "/tmp/cwrap-os-write-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  char* original = arena_new(&c.arena, CONTENTS_LEN, char);
  char* replacement = arena_new(&c.arena, CONTENTS_LEN, char);
  memset(original, 'a', CONTENTS_LEN);
  memset(replacement, 'b', CONTENTS_LEN);
  contract_write(&c, "a.c", (struct Str){original, CONTENTS_LEN});
  const struct Str path = contract_path(&c, "a.c");

#if CWRAP_TEST_OS_MEMORY
  os_memory_fail(c.os, OS_MEMORY_OP_WRITE, path.data, ENOSPC);
  struct Log log = test_log();
  TEST_CHECK(os_write(c.os, path, (struct Str){replacement, CONTENTS_LEN}, S("op"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "op: %s ('%.*s')\n", strerror(ENOSPC), STR_FMT(path)));
#else
  const pid_t child = fork();
  if (child == 0) {
    write_past_size_limit(path, (struct Str){replacement, CONTENTS_LEN});
  }
  int status = 0;
  TEST_CHECK(child > 0 && waitpid(child, &status, 0) == child);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif

  struct Str data = {0};
  struct Log read_log = test_log();
  TEST_CHECK(os_read(c.os, path, CONTENTS_LEN, &c.arena, &data, S("op"), &read_log) == 0);
  TEST_CHECK(str_eq(data, (struct Str){original, CONTENTS_LEN}));
  contract_end(&c);
}

// `os_print` writes each text whole and in call order to the stream it names, and an empty text
// writes nothing. On the POSIX host both streams are captured at their descriptors, and the
// in-memory host keeps what it was given.
static void test_print_writes_each_stream_in_order(void) {
  char root[] = "/tmp/cwrap-os-print.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  int print_a_rc = -1;
  int print_err_rc = -1;
  int print_empty_rc = -1;
  int print_bc_rc = -1;
  struct Str out_text = {0};
  struct Str err_text = {0};
#if !CWRAP_TEST_OS_MEMORY
  struct StreamCapture out;
  struct StreamCapture err;
  char out_buffer[TEST_PRINTED_SIZE];
  char err_buffer[TEST_PRINTED_SIZE];
  if (capture_begin(stdout, &out) != 0) {
    goto cleanup;
  }
  if (capture_begin(stderr, &err) != 0) {
    (void)capture_end(&out, out_buffer, sizeof(out_buffer));
    goto cleanup;
  }
#endif
  print_a_rc = os_print(c.os, 1, S("a"));
  print_err_rc = os_print(c.os, 2, S("err\n"));
  print_empty_rc = os_print(c.os, 1, S(""));
  print_bc_rc = os_print(c.os, 1, S("bc\n"));
#if CWRAP_TEST_OS_MEMORY
  out_text = os_memory_printed(c.os, 1);
  err_text = os_memory_printed(c.os, 2);
#else
  TEST_CHECK(capture_end(&err, err_buffer, sizeof(err_buffer)) == 0);
  TEST_CHECK(capture_end(&out, out_buffer, sizeof(out_buffer)) == 0);
  out_text = str_from_cstr(out_buffer);
  err_text = str_from_cstr(err_buffer);
#endif
  TEST_CHECK(print_a_rc == 0 && print_err_rc == 0 && print_empty_rc == 0 && print_bc_rc == 0);
  TEST_CHECK(str_eq(out_text, S("abc\n")));
  TEST_CHECK(str_eq(err_text, S("err\n")));

#if !CWRAP_TEST_OS_MEMORY
cleanup:
#endif
  contract_end(&c);
}

// A stream that refuses the write makes `os_print` fail with the write's own `errno`. On the POSIX
// host standard output is a read-only descriptor, which fails every write with `EBADF`, and the
// in-memory host is told to fail it the same way.
static void test_print_reports_write_failure(void) {
  char root[] = "/tmp/cwrap-os-print-fail.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
  int print_rc = 0;
  int print_errno = 0;
#if CWRAP_TEST_OS_MEMORY
  os_memory_fail_print(c.os, 1, 0, EBADF);
#else
  struct StreamCapture out;
  if (capture_begin_unwritable(stdout, &out) != 0) {
    goto cleanup;
  }
#endif
  errno = 0;
  print_rc = os_print(c.os, 1, S("a\n"));
  print_errno = errno;
#if !CWRAP_TEST_OS_MEMORY
  TEST_CHECK(capture_end(&out, NULL, 0) == 0);
#endif
  TEST_CHECK(print_rc == -1);
  TEST_CHECK(print_errno == EBADF);

#if !CWRAP_TEST_OS_MEMORY
cleanup:
#endif
  contract_end(&c);
}

// A stream that fails partway keeps the bytes it accepted: the print that crosses the point where
// it fails writes the bytes up to it and then fails with the write's own `errno`, and a later print
// fails without writing. On the POSIX host standard output is a file under a file size limit, and
// the in-memory host is told to fail it at the same length.
static void test_print_keeps_bytes_before_failure(void) {
  char root[] = "/tmp/cwrap-os-print-partial.XXXXXX";
  struct Contract c;
  if (contract_begin(root, &c) != 0) {
    return;
  }
#if CWRAP_TEST_OS_MEMORY
  os_memory_fail_print(c.os, 1, TEST_PRINT_LEN_MAX, EFBIG);
  TEST_CHECK(is_print_cut_at_limit(c.os));
  TEST_CHECK(os_memory_is_printed(c.os, 1, "abcd"));
#else
  const struct Str stdout_path = contract_path(&c, "stdout");
  const pid_t child = fork();
  if (child == 0) {
    print_past_size_limit(stdout_path.data);
  }
  int status = 0;
  TEST_CHECK(child > 0 && waitpid(child, &status, 0) == child);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  TEST_CHECK(contract_is_file(&c, "stdout", S("abcd")));
#endif
  contract_end(&c);
}

TEST_LIST = {
    {"read accepts empty and file at limit", test_read_accepts_empty_and_file_at_limit},
    {"read rejects oversize", test_read_rejects_oversize},
    {"read checks size of extended file", test_read_checks_size_of_extended_file},
    {"read rejects missing and directory", test_read_rejects_missing_and_directory},
    {"read rejects embedded nul", test_read_rejects_embedded_nul},
    {"read rejects path with nul and path at path max",
     test_read_rejects_path_with_nul_and_path_at_path_max},
    {"trailing slash requires directory", test_trailing_slash_requires_directory},
    {"read stdin returns every byte", test_read_stdin_returns_every_byte},
    {"read stdin accepts empty and input at limit",
     test_read_stdin_accepts_empty_and_input_at_limit},
    {"read stdin rejects embedded nul and input over limit",
     test_read_stdin_rejects_embedded_nul_and_input_over_limit},
    {"read stdin reports read error", test_read_stdin_reports_read_error},
    {"read stdin reports error after data", test_read_stdin_reports_error_after_data},
    {"write round trips and replaces", test_write_round_trips_and_replaces},
    {"write rejects directory and path with nul", test_write_rejects_directory_and_path_with_nul},
    {"write failure leaves old file", test_write_failure_leaves_old_file},
    {"print writes each stream in order", test_print_writes_each_stream_in_order},
    {"print reports write failure", test_print_reports_write_failure},
    {"print keeps bytes before failure", test_print_keeps_bytes_before_failure},
    {NULL, NULL},
};
