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
#include "app/cwrap_version.h"

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

// A bare invocation selects implicit standard input.
static void test_no_args_select_stdin(void) {
  char* argv[] = {"cwrap", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_stdin);
  TEST_CHECK(options.paths == NULL);
  TEST_CHECK(options.path_count == 0);
}

// A single `-` selects the same standard-input mode and is removed from the path array.
static void test_dash_selects_stdin(void) {
  char* argv[] = {"cwrap", "-", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_stdin);
  TEST_CHECK(options.paths == NULL);
  TEST_CHECK(options.path_count == 0);
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

// `--width 40` sets the wrapping column from a separate token.
static void test_width_long_flag_separate_value(void) {
  char* argv[] = {"cwrap", "--width", "40", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 40);
}

// `-w 80` sets the wrapping column from a separate token.
static void test_width_short_flag_separate_value(void) {
  char* argv[] = {"cwrap", "-w", "80", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 80);
}

// `-w80` sets the wrapping column from a value attached to the flag.
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

// An `=`-joined value inside a short cluster reaches the one flag it is attached to, leaving the
// valueless flag ahead of it alone. This is the case the rejection in
// `test_attached_value_rejected_on_valueless_short_flags` must not claim. Both flags live in one
// `argv` element, so a check that only asked whether that element contains an `=` would reject
// `--check` here for a value belonging to `--width`.
static void test_width_short_flag_in_cluster_equals_value(void) {
  char* argv[] = {"cwrap", "-cw=40", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
  TEST_CHECK(options.width == 40);
}

// A flag before the file and another after it are both applied in one parse. The
// all-flags-after-the-file placement is `test_options_after_file`.
static void test_mixed_flag_order(void) {
  char* argv[] = {"cwrap", "--check", "a.c", "--width", "40", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.is_check);
  TEST_CHECK(options.width == 40);
  TEST_CHECK(options.path_count == 1);
  TEST_CHECK(strcmp(options.paths[0], "a.c") == 0);
}

// `argv[0]` survives a parse that permutes the rest of the vector, which is the clause
// `cli_dispatch` depends on. It reads `argv[0]` after `cli_parse` returns to name the program in
// the usage text. This command line reorders because the option follows the positional. The test
// fails if a copt update rotates the complete vector. It also fails if `copt_init` stops preserving
// `argv[0]`.
static void test_program_name_survives_permutation(void) {
  char* argv[] = {"cwrap", "a.c", "--width", "40", NULL};
  char* program_name = argv[0];
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(argv[0] == program_name);
  TEST_CHECK(strcmp(argv[0], "cwrap") == 0);
}

// Flags placed after the file are still parsed, and the file stays in the positional tail.
static void test_options_after_file(void) {
  char* argv[] = {"cwrap", "a.c", "--width", "40", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 40);
  TEST_CHECK(options.path_count == 1);
  TEST_CHECK(strcmp(options.paths[0], "a.c") == 0);
}

// `--help` resolves to the help action.
static void test_help_long_flag(void) {
  char* argv[] = {"cwrap", "--help", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
}

// `-h` resolves to the help action.
static void test_help_short_flag(void) {
  char* argv[] = {"cwrap", "-h", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
}

// `--version` resolves to the version action.
static void test_version_long_flag(void) {
  char* argv[] = {"cwrap", "--version", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `-V` resolves to the version action.
static void test_version_short_flag(void) {
  char* argv[] = {"cwrap", "-V", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
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

// An informational flag takes priority over a parse error, and discards the diagnostic that error
// recorded, so `error_message` never describes an outcome other than the one selected. The cluster
// case is here because it looks like an exception and is not. `-Vx` is `-V` alongside the unknown
// option `-x`, the same shape as the two spelled-out cases above, so it prints the version rather
// than reporting the `-x`. A value glued to the flag itself is the shape that does not survive this
// precedence, per `test_attached_value_rejected_on_valueless_short_flags`.
static void test_informational_flag_clears_diagnostic(void) {
  char* help_argv[] = {"cwrap", "--frobnicate", "--help", NULL};
  struct CliOptions options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_HELP);
  TEST_CHECK(options.error_message[0] == '\0');

  char* version_argv[] = {"cwrap", "--frobnicate", "--version", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');

  char* cluster_argv[] = {"cwrap", "-Vx", NULL};
  options = parse(cluster_argv);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// A valid `--version` wins in either order next to a rejected `--version=1`. The rejected spelling
// neither requests the version nor cancels the valid request, so the result does not depend on
// which comes last.
static void test_valid_version_flag_wins_over_rejected_spelling(void) {
  char* valid_first[] = {"cwrap", "--version", "--version=1", NULL};
  struct CliOptions options = parse(valid_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');

  char* rejected_first[] = {"cwrap", "--version=1", "--version", NULL};
  options = parse(rejected_first);
  TEST_CHECK(options.action == CLI_ACTION_VERSION);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `--in-place` and `--check` cannot be combined.
static void test_check_and_in_place_conflict(void) {
  char* argv[] = {"cwrap", "--check", "-i", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      strcmp(options.error_message, "options '--check' and '--in-place' cannot be combined") == 0);
}

// Standard input is a single-input mode, so it cannot be mixed with paths or repeated.
static void test_stdin_cannot_be_combined(void) {
  char* mixed_argv[] = {"cwrap", "a.c", "-", NULL};
  struct CliOptions options = parse(mixed_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "standard input cannot be combined with file inputs") ==
             0);

  char* repeated_argv[] = {"cwrap", "-", "-", NULL};
  options = parse(repeated_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "standard input may be specified only once") == 0);
}

// In-place output needs a path and is rejected for implicit and explicit standard input.
static void test_in_place_rejects_stdin(void) {
  char* implicit_argv[] = {"cwrap", "--in-place", NULL};
  struct CliOptions options = parse(implicit_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      strcmp(options.error_message, "option '--in-place' cannot be used with standard input") == 0);

  char* explicit_argv[] = {"cwrap", "--in-place", "-", NULL};
  options = parse(explicit_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(
      strcmp(options.error_message, "option '--in-place' cannot be used with standard input") == 0);
}

// A non-numeric wrapping column is rejected with a diagnostic naming the offending value.
static void test_invalid_width_rejected(void) {
  char* argv[] = {"cwrap", "--width", "abc", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' must be a positive integer: 'abc'") ==
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
// diagnostic names the offending value. Attached form (`=`) is required so the `-1` is taken as the
// option's value rather than a separate token.
static void test_negative_width_rejected(void) {
  char* argv[] = {"cwrap", "--width=-1", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' must be a positive integer: '-1'") ==
             0);
}

// A wrapping column above the accepted ceiling is rejected. The value at the ceiling is accepted,
// so the boundary itself is pinned rather than just the rejection. `WRAP_COLUMN_MAX` is file-local
// to `cli.c`, so the limit is spelled here as a literal. Changing it must update these two cases
// and the expected message below.
static void test_oversize_width_rejected(void) {
  char* argv[] = {"cwrap", "--width", "10001", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message,
                    "option '--width' exceeds max wrapping column (10000) at 10001") == 0);

  char* at_limit[] = {"cwrap", "--width", "10000", "a.c", NULL};
  options = parse(at_limit);
  TEST_CHECK(options.action == CLI_ACTION_RUN);
  TEST_CHECK(options.width == 10000);
  TEST_CHECK(options.error_message[0] == '\0');
}

// `-w` with no following value is rejected with a diagnostic that it requires a wrapping column,
// naming the short spelling.
static void test_missing_width_short_flag(void) {
  char* argv[] = {"cwrap", "-w", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '-w' requires a wrapping column") == 0);
}

// `--width` with no following value is rejected with a diagnostic that it requires a wrapping
// column, naming the long spelling.
static void test_missing_width_long_flag(void) {
  char* argv[] = {"cwrap", "--width", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option '--width' requires a wrapping column") == 0);
}

// A flag that takes no value rejects an attached `=value` and names the complete token. copt
// matches only the text before `=`. Without this check, it discards the value and enables
// `--check=0`. The test covers all four flags that take no value. `--version` and `--help` clear
// `error_message`, so they could otherwise hide this rejection. The
// `test_width_long_flag_equals_value` test confirms that `--width=N` still works.
static void test_attached_value_rejected_on_valueless_flags(void) {
  char* check_argv[] = {"cwrap", "--check=0", "a.c", NULL};
  struct CliOptions options = parse(check_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_check);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '--check=0'") == 0);

  char* in_place_argv[] = {"cwrap", "--in-place=0", "a.c", NULL};
  options = parse(in_place_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_in_place);
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

// The short form of each valueless flag rejects an attached value and names the complete element.
// copt stops its one-letter comparison at `=`, so `-V=1` matches `V`. Without this check, the short
// form would print the version while the long form failed. An informational flag clears
// `error_message`, so the rejection must prevent `-V` from becoming a version request.
static void test_attached_value_rejected_on_valueless_short_flags(void) {
  char* check_argv[] = {"cwrap", "-c=0", "a.c", NULL};
  struct CliOptions options = parse(check_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_check);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-c=0'") == 0);

  char* in_place_argv[] = {"cwrap", "-i=0", "a.c", NULL};
  options = parse(in_place_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_in_place);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-i=0'") == 0);

  char* help_argv[] = {"cwrap", "-h=1", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-h=1'") == 0);

  char* version_argv[] = {"cwrap", "-V=1", NULL};
  options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-V=1'") == 0);
}

// A rejected attached value ends its short cluster, so the letters after the `=` are not read as
// flags. Otherwise the `V` in `-c=V` would request the version and the `h` in `-c=h` would request
// help, and either would clear the rejection.
static void test_attached_value_rejection_ends_short_cluster(void) {
  char* version_argv[] = {"cwrap", "-c=V", "a.c", NULL};
  struct CliOptions options = parse(version_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(!options.is_check);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-c=V'") == 0);

  char* help_argv[] = {"cwrap", "-c=h", "a.c", NULL};
  options = parse(help_argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "option does not take a value: '-c=h'") == 0);
}

// An unrecognized short option is rejected with a diagnostic naming the option.
static void test_unknown_short_option(void) {
  char* argv[] = {"cwrap", "-x", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '-x'") == 0);
}

// A value attached to an unknown short option ends its cluster, so the `V` in `-x=V` does not
// request the version. The diagnostic names the unknown option rather than the value.
static void test_unknown_short_option_value_ends_cluster(void) {
  char* argv[] = {"cwrap", "-x=V", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '-x'") == 0);
}

// An unrecognized long option is rejected with a diagnostic naming the option.
static void test_unknown_long_option(void) {
  char* argv[] = {"cwrap", "--frobnicate", "a.c", NULL};
  struct CliOptions options = parse(argv);
  TEST_CHECK(options.action == CLI_ACTION_ERROR);
  TEST_CHECK(strcmp(options.error_message, "unknown option '--frobnicate'") == 0);
}

// The generated version accessor returns a non-empty version.
static void test_version_accessor_present(void) {
  TEST_CHECK(cwrap_version_string()[0] != '\0');
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
  const int expected_len =
      snprintf(expected, sizeof(expected), "cwrap %s\n", cwrap_version_string());
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(buf, expected) == 0);
  TEST_CHECK(len == strlen(expected));
  free(buf);
}

// A stream that latched a write error makes `cli_print_version` report `-1` with `errno` set, which
// lets `cli_dispatch` name a reason rather than exiting silently. The reason is `EIO` rather than
// the underlying `EBADF`. A latched error's own `errno` may have been overwritten by the time it is
// noticed, so the contract substitutes a generic I/O failure instead of relaying a stale value. A
// read-mode stream is the deterministic way to reach that branch, because the write fails at the
// `fprintf` while `fflush` itself succeeds.
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
// `EIO` the latched-error branch substitutes. That distinction is why `cli_dispatch` reports a
// reason rather than only an exit code. A closed pipe and a full disk need different responses. The
// test above reaches the `ferror` branch, where the write already failed at `fprintf` and `fflush`
// succeeds. This one reaches the other branch. A pipe with its read end closed is the deterministic
// way there, with `SIGPIPE` ignored so the process survives to return.
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
  // This is `EPIPE`, not the `EIO` substitute. `fflush` observed the failure, so its `errno` is
  // current and passes through untouched.
  TEST_CHECK(reported == EPIPE);
}

// `cli_print_usage` substitutes the caller's program name into the usage line, alongside the option
// content. The name passed here is deliberately *not* `"cwrap"`. With the real program name the
// assertion passes just as well against a hard-coded usage line, so it would prove nothing about
// substitution. `cli_dispatch` passes `argv[0]`, so a user invoking `/usr/local/bin/cwrap --help`
// has to see that path back. The full help text is prose and is deliberately not asserted whole.
// The usage line is the part callers copy.
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
  TEST_CHECK(strstr(buf, "    /opt/bin/mycwrap [options] [<file>...]\n") != NULL);
  TEST_CHECK(strstr(buf, "If no file is given, or the file is '-', read standard input.\n") !=
             NULL);
  TEST_CHECK(strstr(buf, "-w, --width N") != NULL);
  free(buf);
}

// `cli_print_usage` reports a failed write the same way, so neither informational action can exit
// non-zero without a reason.
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
    {"no args select stdin", test_no_args_select_stdin},
    {"dash selects stdin", test_dash_selects_stdin},
    {"check long flag", test_check_long_flag},
    {"check short flag", test_check_short_flag},
    {"in place long flag", test_in_place_long_flag},
    {"in place short flag", test_in_place_short_flag},
    {"width long flag separate value", test_width_long_flag_separate_value},
    {"width short flag separate value", test_width_short_flag_separate_value},
    {"width short flag attached value", test_width_short_flag_attached_value},
    {"width long flag equals value", test_width_long_flag_equals_value},
    {"width short flag in cluster equals value", test_width_short_flag_in_cluster_equals_value},
    {"mixed flag order", test_mixed_flag_order},
    {"program name survives permutation", test_program_name_survives_permutation},
    {"options after file", test_options_after_file},
    {"help long flag", test_help_long_flag},
    {"help short flag", test_help_short_flag},
    {"version long flag", test_version_long_flag},
    {"version short flag", test_version_short_flag},
    {"version flag wins over help", test_version_flag_wins_over_help},
    {"informational flag clears diagnostic", test_informational_flag_clears_diagnostic},
    {"valid version flag wins over rejected spelling",
     test_valid_version_flag_wins_over_rejected_spelling},
    {"check and in place conflict", test_check_and_in_place_conflict},
    {"stdin cannot be combined", test_stdin_cannot_be_combined},
    {"in place rejects stdin", test_in_place_rejects_stdin},
    {"invalid width rejected", test_invalid_width_rejected},
    {"zero width rejected", test_zero_width_rejected},
    {"negative width rejected", test_negative_width_rejected},
    {"oversize width rejected", test_oversize_width_rejected},
    {"missing width short flag", test_missing_width_short_flag},
    {"missing width long flag", test_missing_width_long_flag},
    {"attached value rejected on valueless flags", test_attached_value_rejected_on_valueless_flags},
    {"attached value rejected on valueless short flags",
     test_attached_value_rejected_on_valueless_short_flags},
    {"attached value rejection ends short cluster",
     test_attached_value_rejection_ends_short_cluster},
    {"unknown short option", test_unknown_short_option},
    {"unknown short option value ends cluster", test_unknown_short_option_value_ends_cluster},
    {"unknown long option", test_unknown_long_option},
    {"version accessor present", test_version_accessor_present},
    {"print version writes version line", test_print_version_writes_version_line},
    {"print version reports write failure", test_print_version_reports_write_failure},
    {"print version reports flush failure errno", test_print_version_reports_flush_failure_errno},
    {"print usage names program", test_print_usage_names_program},
    {"print usage reports write failure", test_print_usage_reports_write_failure},
    {NULL, NULL},
};
