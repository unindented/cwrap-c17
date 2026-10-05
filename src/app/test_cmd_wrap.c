#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_wrap.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "runtime/fs.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/** Source the wrap-command tests start from, which rewraps to one line at width 40. */
static const char wrap_fixture_source[] = "// one\n// two\n";

/**
 * @brief Runs the wrap command while capturing both standard streams.
 *
 * Restores both streams before returning.
 *
 * @param options        Wrap options passed to `cmd_wrap_run`.
 * @param stdout_out     Buffer that receives terminated standard output.
 * @param stdout_out_len Size of `stdout_out` in bytes. Must be non-zero.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_wrap_capturing(const struct WrapOptions* options,
                                        char* stdout_out,
                                        size_t stdout_out_len,
                                        char* stderr_out,
                                        size_t stderr_out_len) {
  stdout_out[0] = '\0';
  stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cmd_wrap_run(options);
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed =
        capture_end(&stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
  }
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

/**
 * @brief Runs the wrap command with an unwritable standard output stream.
 *
 * Captures the diagnostic and restores both streams before returning.
 *
 * @param options        Wrap options passed to `cmd_wrap_run`.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode run_wrap_with_unwritable_stdout(const struct WrapOptions* options,
                                                     char* stderr_out,
                                                     size_t stderr_out_len) {
  stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin_unwritable(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cmd_wrap_run(options);
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, stderr_out_len) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, NULL, 0) != 0 || has_plumbing_failed;
  }
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

// A readable source is rewritten to `stdout`, and the command reports success without a diagnostic.
static void test_prints_rewritten_source(void) {
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(file_path, wrap_fixture_source, strlen(wrap_fixture_source)) == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  TEST_CHECK(strcmp(stdout_out, "// one two\n") == 0);
  TEST_CHECK(stderr_out[0] == '\0');

  (void)unlink(file_path);
}

// Check mode reports a source that would change, writes nothing to `stdout`, and leaves the source
// untouched.
static void test_reports_check_change(void) {
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(file_path, wrap_fixture_source, strlen(wrap_fixture_source)) == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
      .is_check = true,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected), "would rewrap '%s'\n", file_path);
  TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, wrap_fixture_source) == 0);
    free(file_data);
  }
}

// In-place mode overwrites a changed source, writes nothing to either stream, and reports success.
static void test_rewrites_in_place(void) {
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(file_path, wrap_fixture_source, strlen(wrap_fixture_source)) == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
      .is_in_place = true,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  TEST_CHECK(stderr_out[0] == '\0');

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, TEST_FILE_LEN_MAX, &file_data, &file_len, NULL, 0) == 0);
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "// one two\n") == 0);
    free(file_data);
  }
}

// A missing input reports the read failure to `stderr`, writes nothing to `stdout`, and returns the
// runtime failure code.
static void test_reports_missing_file(void) {
  char missing_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(missing_path, "", 0) == NULL) {
    return;
  }
  (void)unlink(missing_path);
  char* paths[] = {missing_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read file: %s ('%s')\n",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
}

// Two inputs that both fail in check mode append one diagnostic line each, so neither is lost to
// the other. This is the postcondition `cmd_wrap_run` states for check mode and the reason the
// diagnostic buffer is growable rather than a fixed buffer: an `append_error` that overwrote, or
// that dropped the separator and ran two messages together, would still satisfy every single-file
// test.
static void test_reports_one_line_per_failing_file(void) {
  char missing_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(missing_path, "", 0) == NULL) {
    return;
  }
  (void)unlink(missing_path);
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(file_path, wrap_fixture_source, strlen(wrap_fixture_source)) == NULL) {
    return;
  }
  char* paths[] = {missing_path, file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 2,
      .is_check = true,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  // Paths are reported in argument order. Compared whole, with the separator in the middle, so a
  // dropped newline or a lost line both fail here.
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read file: %s ('%s')\nwould rewrap '%s'\n",
               error_system_message(reason, sizeof(reason), ENOENT), missing_path, file_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
}

// A source that rewrites fine but cannot be written out fails with the stream's own reason, rather
// than exiting 0 when nobody received the output. The reason it reports is what lets the user tell
// a closed pipe from a full disk. A read-only `/dev/null` on `STDOUT_FILENO` is the deterministic
// way to reach that failure: the writes fail, and `cmd_wrap_run` reports it. The diagnostic still
// reaches the captured `stderr`.
static void test_reports_unwritable_stdout(void) {
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  if (init_fixture_file(file_path, wrap_fixture_source, strlen(wrap_fixture_source)) == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_with_unwritable_stdout(&options, stderr_out, sizeof(stderr_out));
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);

  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  // `EBADF`: the writes go to a descriptor opened read-only. The reason is derived from the running
  // libc rather than hardcoded, and the whole line is compared so a reworded prefix fails too.
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write rewritten file to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
}

TEST_LIST = {
    {"prints rewritten source", test_prints_rewritten_source},
    {"reports check change", test_reports_check_change},
    {"rewrites in place", test_rewrites_in_place},
    {"reports missing file", test_reports_missing_file},
    {"reports one line per failing file", test_reports_one_line_per_failing_file},
    {"reports unwritable stdout", test_reports_unwritable_stdout},
    {NULL, NULL},
};
