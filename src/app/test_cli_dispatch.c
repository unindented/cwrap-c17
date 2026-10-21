#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cli.h"
#include "app/cli_dispatch.h"
#include "app/cwrap_version.h"
#include "app/exit_code.h"
#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/**
 * Captured output of one `cli_dispatch` call. Each stream is sized for the most a test here writes
 * to it: `stdout` for the help text around a program name of about 2000 bytes, and `stderr` for a
 * diagnostic that quotes a value of 3000 bytes.
 */
struct DispatchOutput {
  /** Everything written to `stdout`, terminated. */
  char stdout_out[8192];

  /** Everything written to `stderr`, terminated. */
  char stderr_out[4096];
};

/** Source the wrap-command dispatch tests start from, which rewraps to one line at width 40. */
static const char wrap_fixture_source[] = "// one\n// two\n";

/**
 * @brief Runs CLI dispatch while capturing both standard streams.
 *
 * Restores both streams before returning.
 *
 * @param argc         Number of arguments in `argv`.
 * @param argv         Argument vector passed to `cli_dispatch`.
 * @param dispatch_out Captured, terminated `stdout` and `stderr` text.
 * @return The dispatch exit code, or `TEST_PLUMBING_FAILED` on test-plumbing failure.
 */
static enum ExitCode dispatch_capturing(int argc,
                                        char** argv,
                                        struct DispatchOutput* dispatch_out) {
  dispatch_out->stdout_out[0] = '\0';
  dispatch_out->stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cli_dispatch(argc, argv, os_posix());
      has_plumbing_failed = capture_end(&stderr_capture, dispatch_out->stderr_out,
                                        sizeof(dispatch_out->stderr_out)) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, dispatch_out->stdout_out,
                                      sizeof(dispatch_out->stdout_out)) != 0 ||
                          has_plumbing_failed;
  }
  return has_plumbing_failed ? (enum ExitCode)TEST_PLUMBING_FAILED : rc;
}

// `--version` writes the version to `stdout` and reports success.
static void test_version_action_reports_ok(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  const struct Str version = cwrap_version_string();
  char expected[64];
  const int expected_len = snprintf(expected, sizeof(expected), "cwrap %.*s\n", STR_FMT(version));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stdout_out, expected) == 0);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// `--help` writes the usage text to `stdout` and reports success. The program name comes from
// `argv[0]`. Using a name that is deliberately not `"cwrap"` proves the function substitutes it
// rather than hard-coding it. `test_cli.c` pins the text itself, so this test asserts only the
// plumbing.
static void test_help_action_reports_ok(void) {
  char* argv[] = {"/opt/bin/mycwrap", "--help", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(strstr(dispatch_out.stdout_out, "/opt/bin/mycwrap") != NULL);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
}

// `--help` prints whole after a rejected argument as long as the program name, because the command
// line's reservation holds both: `cli_parse` formats its diagnostic there before `--help` drops it,
// and the help text that quotes `argv[0]` is formatted after it. The text is compared whole with
// what `cli_print_usage` prints for the same name on its own.
static void test_help_action_prints_after_long_rejected_argument(void) {
  enum { LONG_ARG_LEN = 2000 };
  static char program_name[1 + LONG_ARG_LEN + 1];
  static char option[sizeof("--width=") + LONG_ARG_LEN];
  program_name[0] = '/';
  memset(program_name + 1, 'p', LONG_ARG_LEN);
  memcpy(option, "--width=", sizeof("--width=") - 1);
  memset(option + sizeof("--width=") - 1, 'z', LONG_ARG_LEN);
  char* argv[] = {program_name, option, "--help", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(3, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');

  static char expected[sizeof(dispatch_out.stdout_out)];
  struct StreamCapture capture;
  if (capture_begin(stdout, &capture) != 0) {
    return;
  }
  const int usage_rc = cli_print_usage(str_from_cstr(program_name), os_posix(), test_arena());
  TEST_CHECK(capture_end(&capture, expected, sizeof(expected)) == 0);
  TEST_CHECK(usage_rc == 0);
  TEST_CHECK(strcmp(dispatch_out.stdout_out, expected) == 0);
}

// The wrap command is dispatched to `cmd_wrap_run` with the parsed options attached, so the path,
// the width, and the in-place flag reach the rewrap rather than being dropped on the way. The
// rewritten file is the only externally visible evidence that all three fields were carried across.
static void test_wrap_command_receives_parsed_options(void) {
  char file_path[] = "/tmp/cwrap-dispatch-source.XXXXXX";
  if (init_fixture_file(file_path, str_from_cstr(wrap_fixture_source)) != 0) {
    return;
  }

  char* argv[] = {"cwrap", "--in-place", "--width=40", file_path, NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_OK);
  TEST_CHECK(dispatch_out.stderr_out[0] == '\0');
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');

  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct Str file_data = {0};
  TEST_CHECK(os_read(os_posix(), str_from_cstr(file_path), TEST_FILE_LEN_MAX, &arena, &file_data,
                     S("failed to read file"), &log) == 0);
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_CHECK(str_eq(file_data, S("// one two\n")));
}

// Check mode is dispatched to `cmd_wrap_run` the same way, so the check flag reaches the rewrap
// rather than being dropped on the way. The would-rewrap diagnostic and the untouched source are
// the only externally visible evidence that it was carried across.
static void test_wrap_command_receives_check_option(void) {
  char file_path[] = "/tmp/cwrap-dispatch-check.XXXXXX";
  if (init_fixture_file(file_path, str_from_cstr(wrap_fixture_source)) != 0) {
    return;
  }

  char* argv[] = {"cwrap", "--check", "--width=40", file_path, NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(4, argv, &dispatch_out) == EXIT_CODE_FAILURE);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
  char expected[2048];
  const int expected_len = snprintf(expected, sizeof(expected), "would rewrap '%s'\n", file_path);
  TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stderr_out, expected) == 0);

  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct Str file_data = {0};
  TEST_CHECK(os_read(os_posix(), str_from_cstr(file_path), TEST_FILE_LEN_MAX, &arena, &file_data,
                     S("failed to read file"), &log) == 0);
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_CHECK(str_eq(file_data, str_from_cstr(wrap_fixture_source)));
}

// A command line that fails to parse reports `EXIT_CODE_USAGE`, and the parser's own diagnostic
// goes to `stderr` rather than `stdout`. The exit code is what nothing else pins: `2` is what a
// shell or CI wrapper branches on to tell a bad invocation from a failed wrap, and `test_cli.c`
// asserts only that `cli_parse` produced the message.
static void test_parse_error_reports_usage_code(void) {
  char* argv[] = {"cwrap", "--bogus", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(2, argv, &dispatch_out) == EXIT_CODE_USAGE);
  TEST_CHECK(strcmp(dispatch_out.stderr_out, "unknown option '--bogus'\n") == 0);
  TEST_CHECK(dispatch_out.stdout_out[0] == '\0');
}

// A diagnostic that quotes a long argument is printed whole, because the command line's reservation
// holds several times the largest `ARG_MAX` in use.
static void test_parse_error_quotes_long_argument_whole(void) {
  enum { LONG_VALUE_LEN = 3000 };
  static char option[sizeof("--width=") + LONG_VALUE_LEN];
  memcpy(option, "--width=", sizeof("--width=") - 1);
  memset(option + sizeof("--width=") - 1, 'x', LONG_VALUE_LEN);
  char* argv[] = {"cwrap", option, "a.c", NULL};
  struct DispatchOutput dispatch_out;
  TEST_CHECK(dispatch_capturing(3, argv, &dispatch_out) == EXIT_CODE_USAGE);
  char expected[sizeof(dispatch_out.stderr_out)];
  const int expected_len =
      snprintf(expected, sizeof(expected), "option '--width' must be a positive integer: '%s'\n",
               option + sizeof("--width=") - 1);
  TEST_ASSERT(expected_len > LONG_VALUE_LEN && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(dispatch_out.stderr_out, expected) == 0);
}

// A `stdout` that cannot be written turns a successful print into `EXIT_CODE_FAILURE` and prints
// nothing to `stderr`. Nothing else in the suite reaches this arm. Other tests make a printer fail,
// but none goes through `cli_dispatch`, so nothing else pins the mapping from a printer's non-zero
// result to an exit code. A read-only descriptor is the deterministic way there, since every write
// to it fails with `EBADF`.
static void test_version_write_failure_reports_failure(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  enum ExitCode rc = (enum ExitCode)TEST_PLUMBING_FAILED;
  bool has_plumbing_failed = true;
  char stderr_out[4096] = "";
  struct StreamCapture stdout_capture;
  struct StreamCapture stderr_capture;
  if (capture_begin_unwritable(stdout, &stdout_capture) == 0) {
    if (capture_begin(stderr, &stderr_capture) == 0) {
      rc = cli_dispatch(2, argv, os_posix());
      has_plumbing_failed = capture_end(&stderr_capture, stderr_out, sizeof(stderr_out)) != 0;
    }
    has_plumbing_failed = capture_end(&stdout_capture, NULL, 0) != 0 || has_plumbing_failed;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(strcmp(stderr_out, "") == 0);
}

TEST_LIST = {
    {"version action reports ok", test_version_action_reports_ok},
    {"help action reports ok", test_help_action_reports_ok},
    {"help action prints after long rejected argument",
     test_help_action_prints_after_long_rejected_argument},
    {"wrap command receives parsed options", test_wrap_command_receives_parsed_options},
    {"wrap command receives check option", test_wrap_command_receives_check_option},
    {"parse error reports usage code", test_parse_error_reports_usage_code},
    {"parse error quotes long argument whole", test_parse_error_quotes_long_argument_whole},
    {"version write failure reports failure", test_version_write_failure_reports_failure},
    {NULL, NULL},
};
