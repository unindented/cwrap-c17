#include "app/cli.h"

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// copt is header-only. This is the single translation unit that defines its implementation. A
// second definition of `COPT_IMPL` would duplicate every copt symbol at link time.
#define COPT_IMPL
#include <copt.h>

#include "app/cwrap_version.h"
#include "core/parse.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

/**
 * Largest wrapping column `--width` accepts.
 *
 * The ceiling is not a safety bound. `fill` compares columns without overflow at any `ptrdiff_t`
 * width, and no allocation scales with it. It exists because past the display width of the longest
 * paragraph every width gives the same output, each paragraph joined onto one line, so a larger
 * value cannot ask for anything a smaller one does not already do.
 *
 * 10000 is deliberately unreachable by legitimate use rather than tuned to an optimum. It sits an
 * order of magnitude above the widest line a terminal or editor displays, under a thousand columns
 * even on a large monitor at a small font, so no real `--width` value meets it. A pathological
 * value fails with a diagnostic naming the limit rather than being accepted silently.
 */
enum { WRAP_COLUMN_MAX = 10000 };

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
 * @param arena   Arena that owns the diagnostic. Must not be `NULL`.
 * @param format  `printf` format of the diagnostic. Must not be `NULL`.
 * @param ...     Arguments that the format converts.
 */
static void record_error(struct CliOptions* options, struct Arena* arena, const char* format, ...)
    __attribute__((format(printf, 3, 4), nonnull(1, 2, 3)));

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
 * A rejection also consumes the rest of the argv element. copt would otherwise go on reading the
 * letters after the `=` as a short cluster, so the `V` in `-v=V` would become a version request and
 * the informational flag would then clear the rejection.
 *
 * @param options Options that receive the diagnostic when a value is attached. Must not be `NULL`.
 * @param opt     copt parser positioned on the option to inspect, before any `copt_arg` call, which
 *                may advance the parser off the element being inspected. On rejection it is left at
 *                the end of that element. Must not be `NULL`.
 * @param argv    Argument vector the parser is walking, used to reach the current element whole.
 *                Must not be `NULL`.
 * @param arena   Arena that owns the diagnostic. Must not be `NULL`.
 * @return `0` when no value is attached, or `-1` with a diagnostic recorded.
 */
static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               struct copt* opt,
                                               char* const* argv,
                                               struct Arena* arena)
    __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Parses and validates the `--width` option argument.
 *
 * Records a diagnostic when the argument is missing, is not a positive integer, or exceeds
 * `WRAP_COLUMN_MAX`. On success stores the wrapping column.
 *
 * @param options Options receiving the wrapping column or a diagnostic. Must not be `NULL`.
 * @param opt     copt parser positioned on the `--width` option. Must not be `NULL`.
 * @param arena   Arena that owns the diagnostic. Must not be `NULL`.
 */
static void cli_parse_width_option(struct CliOptions* options,
                                   struct copt* opt,
                                   struct Arena* arena) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Resolves the positional file arguments into the input mode.
 *
 * Selects standard input when no positional argument is present or the only one is `-`. Otherwise
 * points `options->paths` at the positional tail of `argv`, or records a diagnostic when `-` is
 * repeated or combined with a file.
 *
 * @param options          Options receiving the resolved input and any diagnostic. Must not be
 *                         `NULL`.
 * @param argc             Argument count.
 * @param argv             Argument vector. Must not be `NULL`.
 * @param positional_index Index of the first positional argument, from `copt_idx`.
 * @param arena            Arena that owns the diagnostic. Must not be `NULL`.
 */
static void cli_parse_paths(struct CliOptions* options,
                            int argc,
                            char** argv,
                            ptrdiff_t positional_index,
                            struct Arena* arena) __attribute__((nonnull(1, 3, 5)));

void cli_parse(struct CliOptions* options, int argc, char** argv, struct Arena* arena) {
  *options = (struct CliOptions){.action = CLI_ACTION_RUN, .width = WRAP_COLUMN_DEFAULT};

  bool has_version = false;
  bool has_help = false;

  struct copt opt = copt_init(argc, argv, 1);  // 1 enables argv reordering, not a start index
  copt_set_noargfn(&opt, cli_parse_handle_missing_arg, NULL);
  while (copt_next(&opt)) {
    // A valid informational flag stays requested whatever follows it, and a rejected spelling never
    // requests one, so the outcome does not depend on the order the flags appear in.
    if (copt_opt(&opt, "V|version")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv, arena) == 0) {
        has_version = true;
      }
    } else if (copt_opt(&opt, "h|help")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv, arena) == 0) {
        has_help = true;
      }
    } else if (copt_opt(&opt, "w|width")) {
      cli_parse_width_option(options, &opt, arena);
    } else if (copt_opt(&opt, "i|in-place")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv, arena) == 0) {
        options->is_in_place = true;
      }
    } else if (copt_opt(&opt, "c|check")) {
      if (cli_parse_require_no_attached_value(options, &opt, argv, arena) == 0) {
        options->is_check = true;
      }
    } else {
      record_error(options, arena, "unknown option '%s'", copt_curopt(&opt));
      // A value attached to an unknown option is dropped with the rest of its element, so `-x=V`
      // cannot request the version. The unknown-option diagnostic above is the one that stays,
      // because `record_error` keeps the first.
      (void)cli_parse_require_no_attached_value(options, &opt, argv, arena);
    }
  }

  cli_parse_paths(options, argc, argv, copt_idx(&opt), arena);

  // Version wins over everything. Help then beats a genuine parse error. The parser rejects a mode
  // conflict once no informational flag applies.
  if (has_version || has_help) {
    // An informational action replaces any diagnostic recorded above, so drop it rather than hand
    // back a message that does not describe the outcome. `error_message` is then non-empty exactly
    // when the action is `CLI_ACTION_ERROR`.
    options->error_message = (struct Str){0};
    options->action = has_version ? CLI_ACTION_VERSION : CLI_ACTION_HELP;
  } else if (has_error(options)) {
    options->action = CLI_ACTION_ERROR;
  } else if (options->is_check && options->is_in_place) {
    record_error(options, arena, "options '--check' and '--in-place' cannot be combined");
    options->action = CLI_ACTION_ERROR;
  } else if (options->is_stdin && options->is_in_place) {
    record_error(options, arena, "option '--in-place' cannot be used with standard input");
    options->action = CLI_ACTION_ERROR;
  }
}

int cli_print_version(struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  const struct Str version = cwrap_version_string();
  buf_printf(&text, "cwrap %.*s\n", STR_FMT(version));
  return os_print(os, 1, buf_str(&text));
}

int cli_print_usage(struct Str program_name, struct Os* os, struct Arena scratch) {
  struct Buf text = {.arena = &scratch};
  buf_printf(&text,
             "Rewrap C comments to a wrapping column.\n"
             "\n"
             "USAGE:\n"
             "    %.*s [options] [<file>...]\n"
             "\n"
             "If no file is given, or the file is '-', read standard input.\n"
             "\n"
             "OPTIONS:\n"
             "    -w, --width N   Wrap comments at column N (default: %d)\n"
             "    -i, --in-place  Rewrite files in place\n"
             "    -c, --check     Exit non-zero if any input would change\n"
             "    -h, --help      Print this help and exit\n"
             "    -V, --version   Print version information and exit\n",
             STR_FMT(program_name), WRAP_COLUMN_DEFAULT);
  return os_print(os, 1, buf_str(&text));
}

static bool has_error(const struct CliOptions* options) {
  return options->error_message.len > 0;
}

static void record_error(struct CliOptions* options, struct Arena* arena, const char* format, ...) {
  if (has_error(options)) {
    return;
  }
  // The first pass measures the message and the second writes it into `arena`, as `buf_printf`
  // does. Starting the list twice needs no `va_copy`, and forwarding it to `buf_vprintf` instead
  // makes Clang's analyzer report a false `va_end` on AArch64 Linux.
  va_list args;
  va_start(args, format);
  const int message_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  assert(message_len >= 0);
  char* message = arena_new(arena, (ptrdiff_t)message_len + 1, char);
  va_start(args, format);
  (void)vsnprintf(message, (size_t)message_len + 1, format, args);
  va_end(args);
  options->error_message = (struct Str){message, message_len};
}

static char* cli_parse_handle_missing_arg(const struct copt* opt, void* aux) {
  (void)opt;
  (void)aux;
  return NULL;
}

static int cli_parse_require_no_attached_value(struct CliOptions* options,
                                               struct copt* opt,
                                               char* const* argv,
                                               struct Arena* arena) {
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
  // `-cw=4` is `--check` followed by a wrapping column rather than a value glued to `--check`.
  // Every earlier letter in the cluster is a separate option that received no value. A long option
  // owns any `=` in its own element, so it needs no such test.
  const bool is_short_option = element[1] != '-';
  if (is_short_option && equals[-1] != token[1]) {
    return 0;
  }
  record_error(options, arena, "option does not take a value: '%s'", element);
  // `copt_oarg` consumes whatever follows the current option in its element and ends any short
  // cluster, so the next `copt_next` moves on to the following element. The value must be attached
  // here, so it never reaches for the next element or calls the missing-argument handler. Like
  // `copt_arg`, it does not touch `curopt`, and `token` is not read after it.
  (void)copt_oarg(opt);
  return -1;
}

static void cli_parse_width_option(struct CliOptions* options,
                                   struct copt* opt,
                                   struct Arena* arena) {
  const char* value = copt_arg(opt);
  ptrdiff_t width = 0;
  if (value == NULL) {
    // The option is named by its long spelling whichever one was typed, as the two diagnostics
    // below name it.
    //
    // A negative value also lands here rather than in the positive-integer check below. copt reads
    // `-1` as an option cluster, so `--width` gets no argument at all. `--width -1` therefore
    // reports a missing wrapping column rather than an invalid one. The code accepts this rather
    // than working around it, because claiming `-1` as this option's argument would mean
    // second-guessing the vendored parser's own split between options and arguments.
    record_error(options, arena, "option '--width' requires a wrapping column");
  } else if (parse_size(str_from_cstr(value), &width) != 0 || width == 0) {
    record_error(options, arena, "option '--width' must be a positive integer: '%s'", value);
  } else if (width > WRAP_COLUMN_MAX) {
    // `parse_size` saturates a value past `PTRDIFF_MAX`, whose number is then lost, so the message
    // quotes the argument as typed, without its leading zeros.
    record_error(options, arena, "option '--width' exceeds max wrapping column (%d) at %s",
                 WRAP_COLUMN_MAX, value + strspn(value, "0"));
  } else {
    options->width = width;
  }
}

static void cli_parse_paths(struct CliOptions* options,
                            int argc,
                            char** argv,
                            ptrdiff_t positional_index,
                            struct Arena* arena) {
  if (positional_index < argc) {
    options->paths = argv + positional_index;
    options->path_count = argc - positional_index;
  }

  ptrdiff_t stdin_path_count = 0;
  for (ptrdiff_t i = 0; i < options->path_count; i++) {
    if (strcmp(options->paths[i], "-") == 0) {
      stdin_path_count++;
    }
  }
  if (options->path_count == 0) {
    options->is_stdin = true;
  } else if (stdin_path_count > 1) {
    record_error(options, arena, "standard input may be specified only once");
  } else if (stdin_path_count == 1 && options->path_count > 1) {
    record_error(options, arena, "standard input cannot be combined with file inputs");
  } else if (stdin_path_count == 1) {
    // Normalize explicit `-` to the same representation as implicit standard input.
    options->paths = NULL;
    options->path_count = 0;
    options->is_stdin = true;
  }
}
