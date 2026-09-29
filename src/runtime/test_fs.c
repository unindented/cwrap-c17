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

/**
 * @brief Writes bytes to a unique temporary file.
 *
 * @param text     Bytes to write. May be `NULL` only when `text_len` is zero.
 * @param text_len Number of bytes to write.
 * @return An allocated path the caller must unlink and free, or `NULL` on test-plumbing failure.
 */
static char* write_temp_file(const char* text, size_t text_len) {
  char file_path[] = "/tmp/cwrap-fs.XXXXXX";
  const int fd = mkstemp(file_path);
  TEST_ASSERT(fd >= 0);
  if (fd < 0) {
    return NULL;
  }
  const int close_rc = close(fd);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    (void)unlink(file_path);
    return NULL;
  }
  char* copy = malloc(sizeof(file_path));
  TEST_ASSERT(copy != NULL);
  if (copy == NULL) {
    (void)unlink(file_path);
    return NULL;
  }
  memcpy(copy, file_path, sizeof(file_path));
  const int write_rc = fs_write_file(copy, text, text_len, NULL, 0);
  TEST_CHECK(write_rc == 0);
  if (write_rc != 0) {
    (void)unlink(copy);
    free(copy);
    return NULL;
  }
  return copy;
}

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
  char* file_path = write_temp_file("", 0);
  TEST_ASSERT(file_path != NULL);
  if (file_path == NULL) {
    return;
  }
  char* file_data = NULL;
  size_t file_len = 123;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(file_len == 0);
    TEST_CHECK(strcmp(file_data, "") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
  free(file_path);
}

// `fs_read_file` rejects a missing path and a directory, leaving outputs untouched, and reports the
// two as different reasons rather than one indistinguishable failure.
static void test_read_file_rejects_missing_and_non_regular(void) {
  char* missing_path = write_temp_file("", 0);
  TEST_ASSERT(missing_path != NULL);
  if (missing_path == NULL) {
    return;
  }
  (void)unlink(missing_path);

  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(missing_path, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  char expected[FS_REASON_SIZE];
  TEST_CHECK(strcmp(reason, expected_errno_reason(expected, ENOENT)) == 0);

  // A directory is not a regular file. This is a first-party rejection, not a system error, so it
  // carries its own wording.
  file_data = sentinel;
  file_len = 999;
  reason[0] = '\0';
  TEST_CHECK(fs_read_file("/tmp", &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  TEST_CHECK(strcmp(reason, "not a regular file") == 0);
  free(missing_path);
}

// A file carrying an embedded `NUL` is rejected, leaving outputs untouched, and says so. This is
// the boundary that establishes the `NUL`-free text invariant every downstream `strlen` relies on.
// Accepting it would silently truncate wrapped output at the `NUL`.
static void test_read_file_rejects_embedded_nul(void) {
  const char payload[] = {'a', '\0', 'b'};
  char* file_path = write_temp_file(payload, sizeof(payload));
  TEST_ASSERT(file_path != NULL);
  if (file_path == NULL) {
    return;
  }
  char sentinel[] = "unchanged";
  char* file_data = sentinel;
  size_t file_len = 999;
  char reason[FS_REASON_SIZE] = "";
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(file_data == sentinel);
  TEST_CHECK(file_len == 999);
  // The reason names the policy. No system error occurred and the file reads fine otherwise.
  TEST_CHECK(strcmp(reason, "contains an embedded NUL byte") == 0);
  (void)unlink(file_path);
  free(file_path);
}

// A written file reads back byte-for-byte, and a successful write and a successful read each leave
// the reason untouched.
static void test_write_then_read_round_trips(void) {
  char* file_path = write_temp_file("hello", 5);
  TEST_ASSERT(file_path != NULL);
  if (file_path == NULL) {
    return;
  }
  char reason[FS_REASON_SIZE] = "untouched";
  TEST_ASSERT(chmod(file_path, 0640) == 0);
  TEST_CHECK(fs_write_file(file_path, "hello", 5, reason, sizeof(reason)) == 0);
  TEST_CHECK(strcmp(reason, "untouched") == 0);
  struct stat st;
  TEST_ASSERT(stat(file_path, &st) == 0);
  TEST_CHECK((st.st_mode & 0777) == 0640);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, reason, sizeof(reason)) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(file_len == 5);
    TEST_CHECK(strcmp(file_data, "hello") == 0);
    TEST_CHECK(strcmp(reason, "untouched") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
  free(file_path);
}

// A file that did not exist gets mode `0666` reduced by the umask rather than `mkstemp`'s `0600`,
// and no temporary survives the write. `write_temp_file` pre-creates its file, so this is the only
// test whose write succeeds on the no-existing-file path.
static void test_write_file_creates_file_with_umask_mode(void) {
  char root_dir_template[] = "/tmp/cwrap-fs-mode.XXXXXX";
  const char* root_dir = mkdtemp(root_dir_template);
  TEST_ASSERT(root_dir != NULL);
  if (root_dir == NULL) {
    return;
  }
  char file_path[PATH_MAX];
  const int path_n = snprintf(file_path, sizeof(file_path), "%s/new.c", root_dir);
  TEST_ASSERT(path_n > 0 && (size_t)path_n < sizeof(file_path));
  if (path_n <= 0 || (size_t)path_n >= sizeof(file_path)) {
    (void)rmdir(root_dir);
    return;
  }

  // A umask other than the usual `022` is the case a hardcoded mode would discard. Restore it
  // before asserting, so a failure cannot leak the changed value into later tests.
  const mode_t previous_umask = umask(027);
  const int write_rc = fs_write_file(file_path, "x", 1, NULL, 0);
  (void)umask(previous_umask);

  TEST_CHECK(write_rc == 0);
  struct stat st;
  TEST_ASSERT(stat(file_path, &st) == 0);
  TEST_CHECK((st.st_mode & 07777) == (0666 & ~027));
  (void)unlink(file_path);
  TEST_CHECK(rmdir(root_dir) == 0);
}

// Atomic replacement follows a symbolic link and leaves the link itself in place.
static void test_write_file_follows_symbolic_link(void) {
  char* target_path = write_temp_file("before", strlen("before"));
  TEST_ASSERT(target_path != NULL);
  if (target_path == NULL) {
    return;
  }
  char link_path[] = "/tmp/cwrap-fs-link.XXXXXX";
  const int link_fd = mkstemp(link_path);
  TEST_ASSERT(link_fd >= 0);
  if (link_fd < 0) {
    (void)unlink(target_path);
    free(target_path);
    return;
  }
  const int close_rc = close(link_fd);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    (void)unlink(link_path);
    (void)unlink(target_path);
    free(target_path);
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
  TEST_CHECK(fs_read_file(target_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "after") == 0);
    free(file_data);
  }

  (void)unlink(link_path);
  (void)unlink(target_path);
  free(target_path);
}

// A failed replacement leaves the existing destination byte-for-byte intact.
static void test_write_file_failure_preserves_existing_file(void) {
  char original[2048];
  char replacement[2048];
  memset(original, 'a', sizeof(original));
  memset(replacement, 'b', sizeof(replacement));
  char* file_path = write_temp_file(original, sizeof(original));
  TEST_ASSERT(file_path != NULL);
  if (file_path == NULL) {
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
    free(file_path);
    return;
  }
  int status = 0;
  TEST_ASSERT(waitpid(child, &status, 0) == child);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(file_len == sizeof(original));
    TEST_CHECK(memcmp(file_data, original, sizeof(original)) == 0);
    free(file_data);
  }
  (void)unlink(file_path);
  free(file_path);
}

// `fs_write_file` fails when the parent directory is missing and when the target is itself a
// directory, and reports the two as different system reasons. cwrap does not create parent
// directories, so creating the sibling temporary file fails when the parent is absent.
static void test_write_file_rejects_missing_parent_and_dir_target(void) {
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
    {"read file rejects missing and non-regular", test_read_file_rejects_missing_and_non_regular},
    {"read file rejects embedded nul", test_read_file_rejects_embedded_nul},
    {"write then read round trips", test_write_then_read_round_trips},
    {"write file creates file with umask mode", test_write_file_creates_file_with_umask_mode},
    {"write file follows symbolic link", test_write_file_follows_symbolic_link},
    {"write file failure preserves existing file", test_write_file_failure_preserves_existing_file},
    {"write file rejects missing parent and dir target",
     test_write_file_rejects_missing_parent_and_dir_target},
    {NULL, NULL},
};
