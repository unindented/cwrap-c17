#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime/fs.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/**
 * @brief Formats the system reason an `fs` action reports.
 *
 * Derives the text from the running libc so exact assertions remain portable.
 *
 * @param buf          Buffer that receives the terminated reason.
 * @param error_number System error number to describe.
 * @return `buf` containing the system message.
 */
static const char* expected_errno_reason(char buf[static FS_REASON_SIZE], int error_number) {
  const int reason_len = snprintf(buf, FS_REASON_SIZE, "%s", strerror(error_number));
  TEST_ASSERT(reason_len > 0 && (size_t)reason_len < FS_REASON_SIZE);
  return buf;
}

// The `NULL, 0` reason arguments throughout this file are the documented option, not forgotten
// assertions. `fs`'s contract lets `reason` be `NULL` when `reason_len` is 0, and a fixture write
// that fails is a broken test rather than behavior under test. This is the one suite whose subject
// *is* that diagnostic, so the calls that do assert a reason pass a real buffer. No test here takes
// the `NULL`-reason path on a *failing* call. That is deliberate rather than a gap: the pair
// reaches `error_report`, pinned once at that boundary by `test_report_error_accepts_null_buffer`
// instead of once per `fs` action.

// An empty file reads back as zero bytes with a valid terminator.
static void test_read_file_accepts_empty(void) {
  char file_path[] = "/tmp/cwrap-fs.XXXXXX";
  if (init_fixture_file(file_path, "", 0) == NULL) {
    return;
  }
  char* file_data = NULL;
  size_t file_len = 123;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == 0);
  TEST_CHECK(file_data != NULL && file_data[0] == '\0');
  free(file_data);
  (void)unlink(file_path);
}

// A file exactly at `data_len_max` bytes reads in full. The limit is inclusive.
static void test_read_file_accepts_file_at_limit(void) {
  char file_path[] = "/tmp/cwrap-fs-read-limit.XXXXXX";
  if (init_fixture_file(file_path, "four", strlen("four")) == NULL) {
    return;
  }
  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, strlen("four"), &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == strlen("four"));
  TEST_CHECK(file_data != NULL && strcmp(file_data, "four") == 0);
  free(file_data);
  (void)unlink(file_path);
}

// `fs_read_file` rejects a missing path and a directory, leaving outputs untouched, and reports the
// two as different reasons rather than one indistinguishable failure.
static void test_read_file_rejects_missing_and_non_regular(void) {
  char missing_path[] = "/tmp/cwrap-fs.XXXXXX";
  if (init_fixture_file(missing_path, "", 0) == NULL) {
    return;
  }
  (void)unlink(missing_path);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(missing_path, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  char expected[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOENT)) == 0);

  // A directory is not a regular file. This is a first-party rejection, not a system error, so it
  // carries its own wording.
  file_data = sentinel;
  file_len = 999;
  reason[0] = '\0';
  TEST_CHECK(
      fs_read_file("/tmp", TEST_FILE_LEN_MAX, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);
}

// A FIFO is rejected as not a regular file instead of blocking the open until a writer opens it,
// leaving the outputs untouched.
static void test_read_file_rejects_fifo(void) {
  char fifo_path[] = "/tmp/cwrap-fs-fifo.XXXXXX";
  if (init_fixture_file(fifo_path, "", 0) == NULL) {
    return;
  }
  (void)unlink(fifo_path);
  TEST_ASSERT(mkfifo(fifo_path, 0600) == 0);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(fifo_path, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);
  TEST_CHECK(unlink(fifo_path) == 0);
}

// A file carrying an embedded `NUL` is rejected, leaving outputs untouched, and says so. This is
// the boundary that establishes the `NUL`-free text invariant every downstream `strlen` relies on.
// Accepting it would silently truncate wrapped output at the `NUL`.
static void test_read_file_rejects_embedded_nul(void) {
  const char payload[] = {'a', '\0', 'b'};
  char file_path[] = "/tmp/cwrap-fs.XXXXXX";
  if (init_fixture_file(file_path, payload, sizeof(payload)) == NULL) {
    return;
  }
  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  // The reason names the policy. No system error occurred and the file reads fine otherwise.
  TEST_CHECK(strcmp(reason, "contains an embedded NUL byte") == 0);
  (void)unlink(file_path);
}

// A readable file over `data_len_max` is rejected from its `fstat` size, before anything is
// allocated or read, naming the limit and the size and leaving the outputs untouched. The file is
// sized with `truncate`, so its size is the only thing about it the check can see.
static void test_read_file_rejects_oversize_before_reading(void) {
  char file_path[] = "/tmp/cwrap-fs-read-oversize.XXXXXX";
  if (init_fixture_file(file_path, "", 0) == NULL) {
    return;
  }
  TEST_CHECK(truncate(file_path, (off_t)strlen("five") + 1) == 0);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(
      fs_read_file(file_path, strlen("five"), &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "exceeds max file size (4 bytes) at 5 bytes") == 0);
  (void)unlink(file_path);
}

// A written file reads back byte-for-byte, and a successful write and a successful read each leave
// the reason untouched.
static void test_write_then_read_round_trips(void) {
  char file_path[] = "/tmp/cwrap-fs-roundtrip.XXXXXX";
  if (init_fixture_file(file_path, "", 0) == NULL) {
    return;
  }
  // Unlinked so the write creates the file, rather than leaving a pre-existing one that already
  // held the expected bytes.
  (void)unlink(file_path);

  char reason[FS_REASON_SIZE] = "untouched";
  TEST_CHECK(fs_write_file(file_path, "root", strlen("root"), reason, sizeof(reason)) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &file_data, &file_len, reason,
                          sizeof(reason)) == 0);
  TEST_CHECK(file_len == strlen("root"));
  TEST_CHECK(file_data != NULL && strcmp(file_data, "root") == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  free(file_data);
  (void)unlink(file_path);
}

// A second write replaces a prior file's contents, including bytes past the new end.
static void test_write_file_replaces_contents(void) {
  char file_path[] = "/tmp/cwrap-fs-replace.XXXXXX";
  if (init_fixture_file(file_path, "old contents", strlen("old contents")) == NULL) {
    return;
  }
  static const char replacement[] = {'n', 'e', 'w'};
  TEST_CHECK(fs_write_file(file_path, replacement, sizeof(replacement), NULL, 0) == 0);

  char* data = NULL;
  size_t data_len = 0;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &data, &data_len, NULL, 0) == 0);
  TEST_CHECK(data_len == sizeof(replacement));
  TEST_CHECK(data != NULL && memcmp(data, replacement, sizeof(replacement)) == 0);
  free(data);
  (void)unlink(file_path);
}

// `fs_write_file` creates a new file with `0666` reduced by the process umask rather than
// `mkstemp`'s `0600`, and replacing an existing file keeps that file's mode. No temporary survives
// either write, which the final `rmdir` of the fixture directory checks.
static void test_write_file_applies_umask_and_keeps_existing_mode(void) {
  char root_dir_template[] = "/tmp/cwrap-fs-mode.XXXXXX";
  const char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char created[PATH_MAX];
  const int created_n = snprintf(created, sizeof(created), "%s/created.c", root_dir);
  char existing[PATH_MAX];
  const int existing_n = snprintf(existing, sizeof(existing), "%s/existing.c", root_dir);
  TEST_ASSERT(created_n > 0 && (size_t)created_n < sizeof(created));
  TEST_ASSERT(existing_n > 0 && (size_t)existing_n < sizeof(existing));
  if (created_n <= 0 || (size_t)created_n >= sizeof(created) || existing_n <= 0 ||
      (size_t)existing_n >= sizeof(existing)) {
    (void)rmdir(root_dir);
    return;
  }

  // A umask other than the usual `022` is the case a hardcoded mode would discard. It also must not
  // be `077`, whose result is `mkstemp`'s own `0600` and so could not show the mode was applied.
  // Restore it before asserting, so a failure cannot leak the changed value into later tests.
  const mode_t previous_umask = umask(027);
  TEST_CHECK(fs_write_file(created, "x", 1, NULL, 0) == 0);
  TEST_CHECK(fs_write_file(existing, "x", 1, NULL, 0) == 0);
  TEST_CHECK(chmod(existing, 0640) == 0);
  TEST_CHECK(fs_write_file(existing, "y", 1, NULL, 0) == 0);
  (void)umask(previous_umask);

  struct stat st;
  TEST_CHECK(stat(created, &st) == 0 && (st.st_mode & 07777) == (mode_t)(0666 & ~027));
  TEST_CHECK(stat(existing, &st) == 0 && (st.st_mode & 07777) == 0640);
  (void)unlink(created);
  (void)unlink(existing);
  TEST_CHECK(rmdir(root_dir) == 0);
}

// Atomic replacement follows a symbolic link and leaves the link itself in place.
static void test_write_file_follows_symbolic_link(void) {
  char target_path[] = "/tmp/cwrap-fs.XXXXXX";
  if (init_fixture_file(target_path, "before", strlen("before")) == NULL) {
    return;
  }
  char link_path[] = "/tmp/cwrap-fs-link.XXXXXX";
  if (init_fixture_file(link_path, "", 0) == NULL) {
    (void)unlink(target_path);
    return;
  }
  (void)unlink(link_path);
  TEST_ASSERT(symlink(target_path, link_path) == 0);

  TEST_CHECK(fs_write_file(link_path, "after", strlen("after"), NULL, 0) == 0);
  struct stat link_st;
  TEST_ASSERT(lstat(link_path, &link_st) == 0);
  TEST_CHECK(S_ISLNK(link_st.st_mode));
  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(target_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_data != NULL && strcmp(file_data, "after") == 0);
  free(file_data);

  (void)unlink(link_path);
  (void)unlink(target_path);
}

// A failed replacement leaves the existing destination byte-for-byte intact.
static void test_write_file_failure_preserves_existing_file(void) {
  char original[2048];
  char replacement[2048];
  memset(original, 'a', sizeof(original));
  memset(replacement, 'b', sizeof(replacement));
  char file_path[] = "/tmp/cwrap-fs.XXXXXX";
  if (init_fixture_file(file_path, original, sizeof(original)) == NULL) {
    return;
  }

  const pid_t child = fork();
  TEST_ASSERT(child >= 0);
  if (child == 0) {
    const struct rlimit limit = {.rlim_cur = 1024, .rlim_max = 1024};
    (void)signal(SIGXFSZ, SIG_IGN);
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      _exit(2);
    }
    char reason[FS_REASON_SIZE];
    const int write_rc =
        fs_write_file(file_path, replacement, sizeof(replacement), reason, sizeof(reason));
    _exit(write_rc == -1 ? 0 : 3);
  }
  if (child < 0) {
    (void)unlink(file_path);
    return;
  }
  int status = 0;
  TEST_ASSERT(waitpid(child, &status, 0) == child);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // `original` is over `TEST_FILE_LEN_MAX`, so this read takes its own size as the limit.
  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, sizeof(original), &file_data, &file_len, NULL, 0) == 0);
  TEST_CHECK(file_len == sizeof(original));
  TEST_CHECK(file_data != NULL && memcmp(file_data, original, sizeof(original)) == 0);
  free(file_data);
  (void)unlink(file_path);
}

// `fs_write_file` fails when the parent directory is missing, when a parent component is a regular
// file, and when the target is itself a directory, and reports the three as different system
// reasons. cwrap does not create parent directories, so an absent parent is a failure rather than
// something to fill in. A file in the parent position fails the destination `stat` with `ENOTDIR`
// before any temporary file is created.
static void test_write_file_rejects_missing_or_file_parent_and_dir_target(void) {
  char root_dir_template[] = "/tmp/cwrap-fs-write-fail.XXXXXX";
  const char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }

  char missing_parent_path[PATH_MAX];
  const int missing_n =
      snprintf(missing_parent_path, sizeof(missing_parent_path), "%s/missing/x.c", root_dir);
  TEST_ASSERT(missing_n > 0 && (size_t)missing_n < sizeof(missing_parent_path));
  if (missing_n <= 0 || (size_t)missing_n >= sizeof(missing_parent_path)) {
    (void)rmdir(root_dir);
    return;
  }

  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_write_file(missing_parent_path, "x", 1, reason, sizeof(reason)) == -1);
  char expected[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOENT)) == 0);

  char blocking_file[PATH_MAX];
  const int blocking_n = snprintf(blocking_file, sizeof(blocking_file), "%s/plain.c", root_dir);
  TEST_ASSERT(blocking_n > 0 && (size_t)blocking_n < sizeof(blocking_file));
  if (blocking_n <= 0 || (size_t)blocking_n >= sizeof(blocking_file)) {
    (void)rmdir(root_dir);
    return;
  }
  char through_file[PATH_MAX];
  const int through_n = snprintf(through_file, sizeof(through_file), "%s/x.c", blocking_file);
  TEST_ASSERT(through_n > 0 && (size_t)through_n < sizeof(through_file));
  if (through_n <= 0 || (size_t)through_n >= sizeof(through_file)) {
    (void)rmdir(root_dir);
    return;
  }
  TEST_CHECK(fs_write_file(blocking_file, "x", 1, NULL, 0) == 0);

  reason[0] = '\0';
  TEST_CHECK(fs_write_file(through_file, "x", 1, reason, sizeof(reason)) == -1);
  (void)unlink(blocking_file);
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOTDIR)) == 0);

  char dir_target[PATH_MAX];
  const int target_n = snprintf(dir_target, sizeof(dir_target), "%s/target", root_dir);
  TEST_ASSERT(target_n > 0 && (size_t)target_n < sizeof(dir_target));
  if (target_n <= 0 || (size_t)target_n >= sizeof(dir_target)) {
    (void)rmdir(root_dir);
    return;
  }
  TEST_ASSERT(mkdir(dir_target, 0700) == 0);

  reason[0] = '\0';
  TEST_CHECK(fs_write_file(dir_target, "x", 1, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, EISDIR)) == 0);

  (void)rmdir(dir_target);
  (void)rmdir(root_dir);
}

TEST_LIST = {
    {"read file accepts empty", test_read_file_accepts_empty},
    {"read file accepts file at limit", test_read_file_accepts_file_at_limit},
    {"read file rejects missing and non-regular", test_read_file_rejects_missing_and_non_regular},
    {"read file rejects fifo", test_read_file_rejects_fifo},
    {"read file rejects embedded nul", test_read_file_rejects_embedded_nul},
    {"read file rejects oversize before reading", test_read_file_rejects_oversize_before_reading},
    {"write then read round trips", test_write_then_read_round_trips},
    {"write file replaces contents", test_write_file_replaces_contents},
    {"write file applies umask and keeps existing mode",
     test_write_file_applies_umask_and_keeps_existing_mode},
    {"write file follows symbolic link", test_write_file_follows_symbolic_link},
    {"write file failure preserves existing file", test_write_file_failure_preserves_existing_file},
    {"write file rejects missing or file parent and dir target",
     test_write_file_rejects_missing_or_file_parent_and_dir_target},
    {NULL, NULL},
};
