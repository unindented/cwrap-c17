#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cli.h"

/**
 * @brief Parses a terminated argument vector.
 *
 * @param argv `NULL`-terminated argument vector. Must not be `NULL`.
 * @return The resolved CLI options.
 */
static struct CliOptions parse(char** argv) {
  int argc = 0;
  while (argv[argc] != NULL) {
    argc++;
  }
  struct CliOptions options;
  cli_parse(&options, argc, argv);
  return options;
}

// A file argument with no flags runs at the default width.
static void test_file_argument_runs_at_default_width(void) {
  char* argv[] = {"cwrap", "src/domain/lex.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == WRAP_COLUMN_DEFAULT);
  TEST_CHECK(options.path_count == 1);
  TEST_CHECK(strcmp(options.paths[0], "src/domain/lex.c") == 0);
}

// `--width` stores a parsed positive column.
static void test_width_long_flag(void) {
  char* argv[] = {"cwrap", "--width", "40", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 40);
}

// `-w` is the short form of `--width`.
static void test_width_short_flag(void) {
  char* argv[] = {"cwrap", "-w", "80", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 80);
}

// `-w80` sets the wrapping column from a value attached to the short flag.
static void test_width_short_flag_attached_value(void) {
  char* argv[] = {"cwrap", "-w80", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 80);
}

// `--width=60` sets the wrapping column from an `=`-joined value.
static void test_width_long_flag_equals_value(void) {
  char* argv[] = {"cwrap", "--width=60", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 60);
}

// `--check` enables check mode.
static void test_check_long_flag(void) {
  char* argv[] = {"cwrap", "--check", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
}

// `-c` enables check mode.
static void test_check_short_flag(void) {
  char* argv[] = {"cwrap", "-c", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
}

// `--in-place` enables in-place rewriting.
static void test_in_place_long_flag(void) {
  char* argv[] = {"cwrap", "--in-place", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_in_place);
}

// `-i` enables in-place rewriting.
static void test_in_place_short_flag(void) {
  char* argv[] = {"cwrap", "-i", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_in_place);
}

// An `=`-joined value inside a short cluster reaches the flag it is attached to and leaves the
// valueless flag ahead of it alone.
static void test_width_short_flag_in_cluster_equals_value(void) {
  char* argv[] = {"cwrap", "-cw=40", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
  TEST_CHECK(options.width == 40);
}

// A valueless option before the file and a width option after it are both applied in one parse.
static void test_mixed_flag_order(void) {
  char* argv[] = {"cwrap", "--check", "a.c", "--width", "40", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
  TEST_CHECK(options.width == 40);
  TEST_CHECK(options.path_count == 1);
  TEST_CHECK(strcmp(options.paths[0], "a.c") == 0);
}

// A non-numeric wrapping column is rejected with a diagnostic naming the offending value.
static void test_invalid_width_rejected(void) {
  char* argv[] = {"cwrap", "--width", "nope", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' must be a positive integer: 'nope'") ==
             0);
}

// A wrapping column of zero is rejected, naming the offending value.
static void test_zero_width_rejected(void) {
  char* argv[] = {"cwrap", "--width", "0", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' must be a positive integer: '0'") ==
             0);
}

// A negative wrapping column in attached form reaches `parse_size`'s sign rejection, and the
// diagnostic names the offending value.
static void test_negative_width_rejected(void) {
  char* argv[] = {"cwrap", "--width=-1", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' must be a positive integer: '-1'") ==
             0);
}

// A wrapping column above the accepted ceiling is rejected, while the value at the ceiling is
// accepted. Deriving the expected boundary from `WRAP_COLUMN_MAX` keeps the test tied to the limit.
static void test_oversize_width_rejected(void) {
  char too_large[32];
  const int too_large_len =
      snprintf(too_large, sizeof(too_large), "%zu", (size_t)WRAP_COLUMN_MAX + 1);
  TEST_ASSERT(too_large_len > 0 && (size_t)too_large_len < sizeof(too_large));
  char* argv[] = {"cwrap", "--width", too_large, "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected),
                                    "option '--width' exceeds max wrapping column (%zu) at %zu",
                                    (size_t)WRAP_COLUMN_MAX, (size_t)WRAP_COLUMN_MAX + 1);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(options.error_message, expected) == 0);

  char at_limit[32];
  const int at_limit_len = snprintf(at_limit, sizeof(at_limit), "%zu", (size_t)WRAP_COLUMN_MAX);
  TEST_ASSERT(at_limit_len > 0 && (size_t)at_limit_len < sizeof(at_limit));
  char* accepted_argv[] = {"cwrap", "--width", at_limit, "a.c", NULL};
  options = parse(accepted_argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == WRAP_COLUMN_MAX);
}

// `-w` with no following value is rejected and names the short spelling.
static void test_missing_width_short_flag(void) {
  char* argv[] = {"cwrap", "-w", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '-w' requires a wrapping column") == 0);
}

// `--width` with no following value is rejected and names the long spelling.
static void test_missing_width_long_flag(void) {
  char* argv[] = {"cwrap", "--width", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' requires a wrapping column") == 0);
}

// Flags that take no value reject an attached value. Informational flags must reject before their
// precedence clears the diagnostic.
static void test_attached_value_rejected_on_valueless_flags(void) {
  char* check_argv[] = {"cwrap", "--check=0", "a.c", NULL};
  struct CliOptions options = parse(check_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--check=0'") == 0);

  char* in_place_argv[] = {"cwrap", "--in-place=0", "a.c", NULL};
  options = parse(in_place_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--in-place=0'") == 0);

  char* help_argv[] = {"cwrap", "--help=nope", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--help=nope'") == 0);

  char* version_argv[] = {"cwrap", "--version=nope", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--version=nope'") == 0);
}

// The short form of each valueless flag rejects an attached value too.
static void test_attached_value_rejected_on_valueless_short_flags(void) {
  char* check_argv[] = {"cwrap", "-c=0", "a.c", NULL};
  struct CliOptions options = parse(check_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-c=0'") == 0);

  char* in_place_argv[] = {"cwrap", "-i=0", "a.c", NULL};
  options = parse(in_place_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-i=0'") == 0);

  char* version_argv[] = {"cwrap", "-V=1", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-V=1'") == 0);

  char* help_argv[] = {"cwrap", "-h=1", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-h=1'") == 0);
}

// `--in-place` and `--check` cannot be combined.
static void test_check_and_in_place_conflict(void) {
  char* argv[] = {"cwrap", "--check", "-i", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      strcmp(options.error_message, "options '--check' and '--in-place' cannot be combined") == 0);
}

// A bare invocation with no input is rejected with the usage diagnostic.
static void test_no_args_rejected(void) {
  char* argv[] = {"cwrap", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "no input file specified") == 0);
}

// `--version` resolves to the version action without requiring an input.
static void test_version_long_flag(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `--help` resolves to the help action without requiring an input.
static void test_help_long_flag(void) {
  char* argv[] = {"cwrap", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
}

// `-V` resolves to the version action.
static void test_version_short_flag(void) {
  char* argv[] = {"cwrap", "-V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// `-h` resolves to the help action.
static void test_help_short_flag(void) {
  char* argv[] = {"cwrap", "-h", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
}

// Version wins over help regardless of their order.
static void test_version_flag_wins_over_help(void) {
  char* help_first[] = {"cwrap", "--help", "--version", NULL};
  struct CliOptions options = parse(help_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);

  char* version_first[] = {"cwrap", "--version", "--help", NULL};
  options = parse(version_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
}

// A genuine informational action clears an earlier diagnostic that no longer describes the outcome.
static void test_informational_flag_clears_diagnostic(void) {
  char* argv[] = {"cwrap", "--bogus", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `argv[0]` survives a parse that permutes the rest of the vector.
static void test_program_name_survives_permutation(void) {
  char* argv[] = {"cwrap", "a.c", "--width", "40", NULL};
  char* program_name = argv[0];
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(argv[0] == program_name);
  TEST_CHECK(strcmp(argv[0], "cwrap") == 0);
}

// Options placed after a positional file are still parsed and the positional tail remains intact.
static void test_options_after_file(void) {
  char* argv[] = {"cwrap", "a.c", "--width", "40", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 40);
  TEST_CHECK(options.path_count == 1);
  TEST_CHECK(strcmp(options.paths[0], "a.c") == 0);
}

// An unrecognized long option is rejected with a diagnostic naming the option.
static void test_unknown_long_option(void) {
  char* argv[] = {"cwrap", "--nope", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '--nope'") == 0);
}

// An unrecognized short option is rejected with a diagnostic naming the option.
static void test_unknown_short_option(void) {
  char* argv[] = {"cwrap", "-x", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '-x'") == 0);
}

// The injected `CWRAP_VERSION` macro is defined and non-empty.
static void test_version_macro_present(void) {
  TEST_CHECK(sizeof(CWRAP_VERSION) > 1);
}

// `cli_print_version` writes exactly one `cwrap <version>` line and nothing else.
static void test_print_version_writes_version_line(void) {
  char* buf = NULL;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  TEST_CHECK(cli_print_version(stream) == 0);
  const int rc = fclose(stream);
  TEST_ASSERT(rc == 0);

  char expected[64];
  const int n = snprintf(expected, sizeof(expected), "cwrap %s\n", CWRAP_VERSION);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(buf, expected) == 0);
  TEST_CHECK(len == strlen(expected));
  free(buf);
}

// A stream that latched a write error makes the version printer report failure with `errno` set.
static void test_print_version_reports_write_failure(void) {
  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  errno = 0;
  TEST_CHECK(cli_print_version(stream) == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
}

// When `fflush` itself fails, the write's own `errno` survives instead of being replaced by the
// `EIO` used for an earlier latched error. A pipe with its read end closed reaches that branch,
// with `SIGPIPE` ignored so the process survives to return.
static void test_print_version_reports_flush_failure_errno(void) {
  void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
  int fds[2];
  TEST_ASSERT(pipe(fds) == 0);
  TEST_ASSERT(close(fds[0]) == 0);
  FILE* stream = fdopen(fds[1], "w");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    (void)close(fds[1]);
    (void)signal(SIGPIPE, previous);
    return;
  }

  errno = 0;
  const int rc = cli_print_version(stream);
  const int reported = errno;
  // Once a flush has failed, libc implementations differ on whether `fclose` reports the stream's
  // existing error or succeeds because no buffered output remains. Either result closes it.
  (void)fclose(stream);
  (void)signal(SIGPIPE, previous);

  TEST_CHECK(rc == -1);
  TEST_CHECK(reported == EPIPE);
}

// The usage printer substitutes the caller's program name into the usage line and includes the
// command's options.
static void test_print_usage_names_program(void) {
  char* buf = NULL;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  TEST_CHECK(cli_print_usage(stream, "/opt/bin/mycwrap") == 0);
  const int rc = fclose(stream);
  TEST_ASSERT(rc == 0);
  TEST_CHECK(strstr(buf, "    /opt/bin/mycwrap [options] <file>...\n") != NULL);
  TEST_CHECK(strstr(buf, "-w, --width N") != NULL);
  free(buf);
}

// The usage printer reports a failed write the same way, so neither informational action can fail
// without a reason.
static void test_print_usage_reports_write_failure(void) {
  FILE* stream = fopen("/dev/null", "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  errno = 0;
  TEST_CHECK(cli_print_usage(stream, "cwrap") == -1);
  TEST_CHECK(errno == EIO);
  const int close_rc = fclose(stream);
  TEST_CHECK(close_rc == 0);
}

TEST_LIST = {
    {"file argument runs at default width", test_file_argument_runs_at_default_width},
    {"width long flag", test_width_long_flag},
    {"width short flag", test_width_short_flag},
    {"width short flag attached value", test_width_short_flag_attached_value},
    {"width long flag equals value", test_width_long_flag_equals_value},
    {"check long flag", test_check_long_flag},
    {"check short flag", test_check_short_flag},
    {"in place long flag", test_in_place_long_flag},
    {"in place short flag", test_in_place_short_flag},
    {"width short flag in cluster equals value", test_width_short_flag_in_cluster_equals_value},
    {"mixed flag order", test_mixed_flag_order},
    {"invalid width rejected", test_invalid_width_rejected},
    {"zero width rejected", test_zero_width_rejected},
    {"negative width rejected", test_negative_width_rejected},
    {"oversize width rejected", test_oversize_width_rejected},
    {"missing width short flag", test_missing_width_short_flag},
    {"missing width long flag", test_missing_width_long_flag},
    {"attached value rejected on valueless flags", test_attached_value_rejected_on_valueless_flags},
    {"attached value rejected on valueless short flags",
     test_attached_value_rejected_on_valueless_short_flags},
    {"check and in place conflict", test_check_and_in_place_conflict},
    {"no args rejected", test_no_args_rejected},
    {"version long flag", test_version_long_flag},
    {"help long flag", test_help_long_flag},
    {"version short flag", test_version_short_flag},
    {"help short flag", test_help_short_flag},
    {"version flag wins over help", test_version_flag_wins_over_help},
    {"informational flag clears diagnostic", test_informational_flag_clears_diagnostic},
    {"program name survives permutation", test_program_name_survives_permutation},
    {"options after file", test_options_after_file},
    {"unknown long option", test_unknown_long_option},
    {"unknown short option", test_unknown_short_option},
    {"version macro present", test_version_macro_present},
    {"print version writes version line", test_print_version_writes_version_line},
    {"print version reports write failure", test_print_version_reports_write_failure},
    {"print version reports flush failure errno", test_print_version_reports_flush_failure_errno},
    {"print usage names program", test_print_usage_names_program},
    {"print usage reports write failure", test_print_usage_reports_write_failure},
    {NULL, NULL},
};
