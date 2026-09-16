#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cli_dispatch.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "runtime/fs.h"

/**
 * Captured output of one `cli_dispatch` call. Both streams are sized for the whole usage text,
 * which is the longest thing any dispatch path writes.
 */
struct DispatchOutput {
  /** Everything written to `stdout`, terminated. */
  char stdout_out[8192];

  /** Everything written to `stderr`, terminated. */
  char stderr_out[4096];
};

/**
 * @brief Reads an anonymous capture stream.
 *
 * @param capture      Stream to rewind and read. Must not be `NULL`.
 * @param text_out     Buffer that receives terminated captured text.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int read_capture(FILE* capture, char* text_out, size_t text_out_len) {
  TEST_ASSERT(text_out_len > 0);
  if (fseek(capture, 0, SEEK_SET) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  const size_t text_len = fread(text_out, 1, text_out_len - 1, capture);
  text_out[text_len] = '\0';
  if (text_len == text_out_len - 1) {
    const int trailing = fgetc(capture);
    TEST_CHECK(trailing == EOF);
    if (trailing != EOF) {
      return -1;
    }
  }
  if (ferror(capture) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

/**
 * @brief Runs CLI dispatch while capturing both standard streams.
 *
 * Redirects descriptors so both streams can be restored after the call. Clears stream errors left
 * by write-failure paths before returning.
 *
 * @param argc         Number of arguments in `argv`.
 * @param argv         Argument vector passed to `cli_dispatch`.
 * @param dispatch_out Captured, terminated `stdout` and `stderr` text.
 * @return The dispatch exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode dispatch_capturing(int argc,
                                        char** argv,
                                        struct DispatchOutput* dispatch_out) {
  dispatch_out->stdout_out[0] = '\0';
  dispatch_out->stderr_out[0] = '\0';

  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  FILE* stdout_capture = NULL;
  FILE* stderr_capture = NULL;

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    return rc;
  }

  saved_stdout = dup(STDOUT_FILENO);
  TEST_CHECK(saved_stdout >= 0);
  if (saved_stdout < 0) {
    goto cleanup;
  }
  saved_stderr = dup(STDERR_FILENO);
  TEST_CHECK(saved_stderr >= 0);
  if (saved_stderr < 0) {
    goto cleanup;
  }
  stdout_capture = tmpfile();
  TEST_ASSERT(stdout_capture != NULL);
  if (stdout_capture == NULL) {
    goto cleanup;
  }
  stderr_capture = tmpfile();
  TEST_ASSERT(stderr_capture != NULL);
  if (stderr_capture == NULL) {
    goto cleanup;
  }

  {
    const int stdout_redirect_rc = dup2(fileno(stdout_capture), STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cli_dispatch(argc, argv);
    const int captured_stdout_flush_rc = fflush(stdout);
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stdout_flush_rc == 0);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stdout_flush_rc != 0 || captured_stderr_flush_rc != 0;
  }

cleanup:
  if (saved_stdout >= 0) {
    const int restore_rc = dup2(saved_stdout, STDOUT_FILENO);
    TEST_CHECK(restore_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDOUT_FILENO;
    const int close_rc = close(saved_stdout);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (saved_stderr >= 0) {
    const int restore_rc = dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(restore_rc == STDERR_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDERR_FILENO;
    const int close_rc = close(saved_stderr);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  clearerr(stdout);
  clearerr(stderr);

  if (stdout_capture != NULL) {
    has_plumbing_failed = read_capture(stdout_capture, dispatch_out->stdout_out,
                                       sizeof(dispatch_out->stdout_out)) != 0 ||
                          has_plumbing_failed;
    const int close_rc = fclose(stdout_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (stderr_capture != NULL) {
    has_plumbing_failed = read_capture(stderr_capture, dispatch_out->stderr_out,
                                       sizeof(dispatch_out->stderr_out)) != 0 ||
                          has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  return has_plumbing_failed ? (enum ExitCode)EXIT_CODE_VALUE_MAX : rc;
}

/**
 * @brief Creates a source fixture for wrap-command dispatch tests.
 *
 * @param file_path Writable `mkstemp` template. Receives the created path.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int init_wrap_fixture(char file_path[static 1]) {
  const int fd = mkstemp(file_path);
  TEST_ASSERT(fd >= 0);
  if (fd < 0) {
    return -1;
  }
  const int close_rc = close(fd);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    (void)unlink(file_path);
    return -1;
  }
  const int rc = fs_write_file(file_path, "// one\n// two\n", strlen("// one\n// two\n"), NULL, 0);
  TEST_CHECK(rc == 0);
  if (rc != 0) {
    (void)unlink(file_path);
  }
  return rc;
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  // Composed rather than concatenated with the macro: `"cwrap " CWRAP_VERSION` is a string
  // concatenation cppcheck cannot parse without the build's `-DCWRAP_VERSION`.
  char expected[64];
  const int n = snprintf(expected, sizeof(expected), "cwrap %s\n", CWRAP_VERSION);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stdout_out, expected) == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// `--help` writes usage to `stdout` and reports success. The program name comes from `argv[0]`.
// Using a deliberately different name proves the function substitutes it rather than hard-coding
// `cwrap`. `test_cli.c` pins the text itself, so this test asserts only the plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mycwrap", "--help", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strstr(dispatch_out.stdout_out, "/opt/bin/mycwrap") != NULL);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// The wrap command receives the parsed path, width, and in-place option rather than dropping them
// at the dispatch boundary. The changed file is the externally visible evidence that all three
// fields reached `cmd_wrap_run`.
static void test_wrap_command_receives_parsed_options(void) {
  char file_path[] = "/tmp/cwrap-dispatch-source.XXXXXX";
  if (init_wrap_fixture(file_path) != 0) {
    return;
  }

  char* argv[] = {"cwrap", "--in-place", "--width=40", file_path, NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "// one two\n") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
}

// Check mode also reaches the wrap command. It reports a source that would change and leaves that
// source untouched, proving `is_check` is carried across the dispatch boundary.
static void test_wrap_command_receives_check_option(void) {
  char file_path[] = "/tmp/cwrap-dispatch-check.XXXXXX";
  if (init_wrap_fixture(file_path) != 0) {
    return;
  }

  char* argv[] = {"cwrap", "--check", "--width=40", file_path, NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_FAILURE);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
  char expected[4096];
  const int n = snprintf(expected, sizeof(expected), "would rewrap '%s'\n", file_path);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stderr_out, expected) == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "// one\n// two\n") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. `test_cli.c` asserts that `cli_parse` produced the
// message; this test pins the dispatch mapping and stream routing.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"cwrap", "--bogus", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_USAGE);
  TEST_CHECK(strcmp(dispatch_out.stderr_out, "unknown option '--bogus'\n") == 0);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
}

// A `stdout` that cannot be written turns a successful version action into `EXIT_CODE_FAILURE` and
// names both the subject and the stream's own reason. A read-only descriptor is deterministic: the
// write fails inside the printer's `fflush`, and the captured `stderr` remains writable.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  int unwritable = -1;
  int sink = -1;
  FILE* stderr_capture = NULL;

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    return;
  }

  saved_stdout = dup(STDOUT_FILENO);
  TEST_CHECK(saved_stdout >= 0);
  if (saved_stdout < 0) {
    goto cleanup;
  }
  saved_stderr = dup(STDERR_FILENO);
  TEST_CHECK(saved_stderr >= 0);
  if (saved_stderr < 0) {
    goto cleanup;
  }
  unwritable = open("/dev/null", O_RDONLY);
  TEST_CHECK(unwritable >= 0);
  if (unwritable < 0) {
    goto cleanup;
  }
  sink = open("/dev/null", O_WRONLY);
  TEST_CHECK(sink >= 0);
  if (sink < 0) {
    goto cleanup;
  }
  stderr_capture = tmpfile();
  TEST_ASSERT(stderr_capture != NULL);
  if (stderr_capture == NULL) {
    goto cleanup;
  }

  {
    const int stdout_redirect_rc = dup2(unwritable, STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cli_dispatch(2, argv);
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stderr_flush_rc != 0;

    // The failed write left the version text in `stdout`'s buffer. Drain it into a writable sink
    // before restoring the real descriptor, or the next successful flush prints it to the harness.
    const int sink_redirect_rc = dup2(sink, STDOUT_FILENO);
    TEST_CHECK(sink_redirect_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || sink_redirect_rc != STDOUT_FILENO;
    if (sink_redirect_rc == STDOUT_FILENO) {
      clearerr(stdout);
      const int drain_rc = fflush(stdout);
      TEST_CHECK(drain_rc == 0);
      has_plumbing_failed = has_plumbing_failed || drain_rc != 0;
    }
  }

cleanup:
  if (saved_stdout >= 0) {
    const int restore_rc = dup2(saved_stdout, STDOUT_FILENO);
    TEST_CHECK(restore_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDOUT_FILENO;
    const int close_rc = close(saved_stdout);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (saved_stderr >= 0) {
    const int restore_rc = dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(restore_rc == STDERR_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDERR_FILENO;
    const int close_rc = close(saved_stderr);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  clearerr(stdout);
  clearerr(stderr);
  if (unwritable >= 0) {
    const int close_rc = close(unwritable);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (sink >= 0) {
    const int close_rc = close(sink);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }

  char stderr_out[4096] = "";
  if (stderr_capture != NULL) {
    has_plumbing_failed =
        read_capture(stderr_capture, stderr_out, sizeof(stderr_out)) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write version to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"wrap command receives parsed options", test_wrap_command_receives_parsed_options},
    {"wrap command receives check option", test_wrap_command_receives_check_option},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {NULL, NULL},
};
