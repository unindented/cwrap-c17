#include "app/cli.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

// copt is header-only. This is the single translation unit that defines its implementation. A
// second definition of `COPT_IMPL` would duplicate every copt symbol at link time.
#define COPT_IMPL
#include <copt.h>

#include "core/parse.h"

// The build normally injects this from the release tag. The fallback keeps a bare `cc` invocation
// of this file compiling.
#ifndef CWRAP_VERSION
#define CWRAP_VERSION "0.0.0-dev"
#endif

/**
 * @brief Reports whether a parse error has already been recorded.
 *
 * @param options Options whose `error_message` is inspected. Must not be `NULL`.
 * @return `true` when a diagnostic has been recorded, `false` otherwise.
 */
static bool has_error(const struct CliOptions* options) __attribute__((nonnull(1)));

/**
 * @brief Records the first parse error and marks the command line as rejected.
 *
 * This keeps only the first error, so the user sees the earliest failure. It ignores later calls
 * once `has_error` reports a recorded diagnostic.
 *
 * @param options Options whose `error_message` receives the diagnostic. Must not be `NULL`.
 * @param fmt     `printf`-style format string. Must not be `NULL`.
 * @param ...     Arguments for `fmt`.
 */
static void record_error(struct CliOptions* options, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Reports a missing option argument as `NULL` instead of letting copt exit.
 *
 * This is installed as copt's no-arg callback. Returning `NULL` lets `cli_parse` report the missing
 * argument itself.
 *
 * @param opt Current copt option (unused).
 * @param aux Callback data (unused).
 * @return `NULL` always.
 */
static char* cli_parse_handle_missing_arg(const struct copt* opt, void* aux);

/**
 * @brief Requires that an option taking no value has none attached.
 *
 * copt matches a long option by the text before any `=`, so `--check=0` matches `check`. copt then
 * silently drops the `0`, because nothing asks for an argument the option does not have. A user who
 * asks for check mode off would get it on. Nothing else reads the value, so nothing else can notice
 * it.
 *
 * The caller must branch on the return value, not just the recorded diagnostic. An informational
 * flag clears `error_message` by design, so a `--version=x` that still set `has_version` would have
 * its rejection wiped and exit `0`. Treating the malformed spelling as not a version request
 * prevents that.
 *
 * copt matches a short option the same way, clamping its one-letter comparison at an `=`, so `-V=1`
 * matches `V`. The check therefore inspects the raw argv element rather than `copt_curopt`, which
 * for a short option is copt's own two-byte `-x` scratch string and can never hold an `=`. Reading
 * only that would leave `-V=1` accepted as a version request while `--version=1` was rejected.
 *
 * @param options Options that receive the diagnostic when a value is attached. Must not be `NULL`.
 * @param opt     copt parser positioned on the option to inspect, before any `copt_arg` call, which
 *                may advance the parser off the element being inspected. Must not be `NULL`.
 * @param argv    Argument vector the parser is walking, used to reach the current element whole.
 *                Must not be `NULL`.
 * @return `0` when no value is attached, or `-1` with a diagnostic recorded.
 */
static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               const struct copt* opt,
                                               char** argv) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Parses and validates the `--width` option argument.
 *
 * Records a diagnostic when the argument is missing, is not a positive integer, or exceeds
 * `WRAP_COLUMN_MAX`. On success stores the wrapping column.
 *
 * @param options Options receiving the width or a diagnostic. Must not be `NULL`.
 * @param opt     copt parser positioned on the `--width` option. Must not be `NULL`.
 */
static void cli_parse_width_option(struct CliOptions* options, struct copt* opt)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Flushes `stream` and reports whether any write to it failed, leaving a reason in `errno`.
 *
 * The check needs both halves. `fflush` reports a write that fails now, when the buffered bytes
 * reach the stream. `ferror` catches one the stream latched during an earlier unchecked `fprintf`.
 * Neither alone suffices, because `stdout` to a pipe or a file is fully buffered, so an earlier
 * `fprintf` can succeed while the write does not.
 *
 * The function always leaves `errno` describing the failure, which lets `cli_dispatch` report a
 * reason rather than only an exit code. A latched error's own `errno` may have been overwritten
 * since, so that case substitutes `EIO` rather than relay a stale value as the cause.
 *
 * @param stream Stream to flush and inspect. Must not be `NULL`.
 * @return `0` when every write succeeded, or `-1` with `errno` set to the reason.
 */
static int flush_stream(FILE* stream) __attribute__((nonnull(1)));

void cli_parse(struct CliOptions* options, int argc, char** argv) {
  *options = (struct CliOptions){
      .action = CLI_ACTION_RUN,
      .width = WRAP_COLUMN_DEFAULT,
  };

  bool has_version = false;
  bool has_help = false;

  struct copt opt = copt_init(argc, argv, 1);  // 1 enables argv reordering, not a start index
  copt_set_noargfn(&opt, cli_parse_handle_missing_arg, NULL);
  while (copt_next(&opt)) {
    if (copt_opt(&opt, "V|version")) {
      has_version = cli_parse_require_no_attached_value(options, &opt, argv) == 0;
    } else if (copt_opt(&opt, "h|help")) {
      has_help = cli_parse_require_no_attached_value(options, &opt, argv) == 0;
    } else if (copt_opt(&opt, "w|width")) {
      cli_parse_width_option(options, &opt);
    } else if (copt_opt(&opt, "i|in-place")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv) == 0) {
        options->is_in_place = true;
      }
    } else if (copt_opt(&opt, "c|check")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv) == 0) {
        options->is_check = true;
      }
    } else {
      record_error(options, "unknown option '%s'", copt_curopt(&opt));
    }
  }

  const int positional_index = copt_idx(&opt);
  if (positional_index < argc) {
    options->paths = argv + positional_index;
    options->path_count = argc - positional_index;
  }

  // Version wins over everything. Help then beats a genuine parse error. The parser rejects a bare
  // invocation with no input once no informational flag applies.
  if (has_version || has_help) {
    // An informational action replaces any diagnostic recorded above, so drop it rather than hand
    // back a message that does not describe the outcome. `error_message` is then non-empty exactly
    // when the action is `CLI_ACTION_ERROR`.
    options->error_message[0] = '\0';
    options->action = has_version ? CLI_ACTION_VERSION : CLI_ACTION_HELP;
  } else if (has_error(options)) {
    options->action = CLI_ACTION_ERROR;
  } else if (options->is_check && options->is_in_place) {
    record_error(options, "options '--check' and '--in-place' cannot be combined");
    options->action = CLI_ACTION_ERROR;
  } else if (options->path_count == 0) {
    record_error(options, "no input file specified");
    options->action = CLI_ACTION_ERROR;
  }
}

int cli_print_version(FILE* stream) {
  fprintf(stream, "cwrap %s\n", CWRAP_VERSION);
  return flush_stream(stream);
}

int cli_print_usage(FILE* stream, const char* program_name) {
  fprintf(stream,
          "Rewrap C comments to a wrapping column.\n"
          "\n"
          "USAGE:\n"
          "    %s [options] <file>...\n"
          "\n"
          "OPTIONS:\n"
          "    -w, --width N   Wrap comments at column N (default: %d)\n"
          "    -i, --in-place  Rewrite files in place\n"
          "    -c, --check     Exit non-zero if any file would change\n"
          "    -h, --help      Print this help and exit\n"
          "    -V, --version   Print version information and exit\n",
          program_name, WRAP_COLUMN_DEFAULT);
  // The `fflush` plus `ferror` pair, for the reason given on `flush_stream`.
  return flush_stream(stream);
}

static bool has_error(const struct CliOptions* options) {
  return options->error_message[0] != '\0';
}

static void record_error(struct CliOptions* options, const char* fmt, ...) {
  if (has_error(options)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  error_report_va(options->error_message, sizeof(options->error_message), fmt, ap);
  va_end(ap);
}

static char* cli_parse_handle_missing_arg(const struct copt* opt, void* aux) {
  (void)opt;
  (void)aux;
  return NULL;
}

static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               const struct copt* opt,
                                               char** argv) {
  const char* token = copt_curopt(opt);
  // The whole element, which for a short option is the entire cluster rather than the one letter
  // `token` names. copt documents `copt_idx` for use after the option loop, where it gives the
  // first positional argument. Called during the loop it reports the element being parsed, which is
  // the one needed here. copt's own contract does not promise that though.
  const char* element = argv[copt_idx(opt)];
  const char* equals = strchr(element, '=');
  if (equals == NULL) {
    return 0;
  }
  // Within a short cluster the value belongs to the single letter directly before the `=`, so
  // `-ci=1` is `--check` followed by an in-place value rather than a value glued to `--check`.
  // Every earlier letter in the cluster is a separate option that received no value. A long option
  // owns any `=` in its own element, so it needs no such test.
  const bool is_short_option = element[1] != '-';
  if (is_short_option && equals[-1] != token[1]) {
    return 0;
  }
  // The whole element trails the cause. It is a raw argument, so it is unbounded and must not be
  // able to truncate the reason away.
  record_error(options, "option does not take a value: '%s'", element);
  return -1;
}

static void cli_parse_width_option(struct CliOptions* options, struct copt* opt) {
  const char* value = copt_arg(opt);
  size_t width = 0;
  if (value == NULL) {
    // `copt_curopt` returns a pointer into `opt`'s own scratch storage for a short option. Format
    // it before the loop advances, while that spelling is still valid.
    record_error(options, "option '%s' requires a wrapping column", copt_curopt(opt));
  } else if (parse_size(value, &width) != 0 || width == 0) {
    record_error(options, "option '--width' must be a positive integer: '%s'", value);
  } else if (width > (size_t)WRAP_COLUMN_MAX) {
    record_error(options, "option '--width' exceeds max wrapping column (%zu) at %zu",
                 (size_t)WRAP_COLUMN_MAX, width);
  } else {
    options->width = width;
  }
}

static int flush_stream(FILE* stream) {
  if (fflush(stream) != 0) {
    return -1;
  }
  if (ferror(stream) != 0) {
    errno = EIO;
    return -1;
  }
  return 0;
}
