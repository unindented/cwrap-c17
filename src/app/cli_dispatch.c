#include "app/cli_dispatch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/cli.h"
#include "app/cmd_wrap.h"
#include "app/exit_code.h"
#include "core/error.h"

/**
 * @brief Maps a printer's result to an exit code, reporting the stream's own reason on failure.
 *
 * On failure, reports the stream's `errno` rather than only returning failure. `cli_dispatch` uses
 * the reason to distinguish output failures from command failures.
 *
 * A closed stdout pipe looks like a common path into this function, but it usually is not. With the
 * default `SIGPIPE` disposition, the process dies of signal 13 inside `fflush` and never reaches
 * here. A broken-pipe diagnostic appears only when the caller ignored `SIGPIPE` and that
 * disposition survived `exec`.
 *
 * Reading `errno` after the printer returns is valid by contract. `cli_print_version` and
 * `cli_print_usage` both document that they set it. If the stream had latched an error earlier and
 * the original `errno` may have been overwritten, they set `EIO` instead.
 *
 * @param print_rc Result of the printer call.
 * @param subject  What was being written, named in the diagnostic, such as `version` or `usage`.
 *                 Must not be `NULL`.
 * @return `EXIT_CODE_OK` when `print_rc` is 0, or `EXIT_CODE_FAILURE` after reporting the reason.
 */
static enum ExitCode exit_code_from_print(int print_rc, const char* subject)
    __attribute__((nonnull(2)));

enum ExitCode cli_dispatch(int argc, char** argv) {
  struct CliOptions options;
  cli_parse(&options, argc, argv);

  switch (options.action) {
    case CLI_ACTION_VERSION:
      return exit_code_from_print(cli_print_version(stdout), "version");
    case CLI_ACTION_HELP: {
      // `CLI_ACTION_HELP` means `cli_parse` saw `--help`, so `argc >= 2` and `argv[0]` is a string.
      const char* program_name = argv[0];
      return exit_code_from_print(cli_print_usage(stdout, program_name), "usage");
    }
    case CLI_ACTION_ERROR:
      fprintf(stderr, "%s\n", options.error_message);
      return EXIT_CODE_USAGE;
    case CLI_ACTION_RUN: {
      const struct WrapOptions wrap_options = {
          .width = options.width,
          .paths = options.paths,
          .path_count = options.path_count,
          .is_stdin = options.is_stdin,
          .is_in_place = options.is_in_place,
          .is_check = options.is_check,
      };
      return cmd_wrap_run(&wrap_options);
    }
  }
  // Unreachable: the switch covers every `enum CliAction`. This is deliberately not a `default:`
  // case, so `-Wswitch` still fails the build when someone adds an action without a case here. A
  // value outside the enum means memory corruption, which there is no sensible exit code for.
  abort();
}

static enum ExitCode exit_code_from_print(int print_rc, const char* subject) {
  if (print_rc == 0) {
    return EXIT_CODE_OK;
  }
  // This mapping reports only real write failures, because both printers end in an `fflush` plus
  // `ferror` check. A printer added without that pair could lose output to a closed pipe or full
  // disk and still claim success here.
  char message[ERROR_MESSAGE_SIZE];
  fprintf(stderr, "failed to write %s to 'stdout': %s\n", subject,
          error_system_message(message, sizeof(message), errno));
  return EXIT_CODE_FAILURE;
}
