#ifndef CWRAP_CLI_H
#define CWRAP_CLI_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Os;

/** Wrapping column used when `--width` is omitted. */
enum { WRAP_COLUMN_DEFAULT = 100 };

/** Action the parsed command line selected, resolved by precedence in `cli_parse`. */
enum CliAction {
  /** Print the version and exit successfully. Wins over every other flag. */
  CLI_ACTION_VERSION,

  /** Print usage help and exit successfully. Wins over parse errors but not version. */
  CLI_ACTION_HELP,

  /** Rewrap the selected input. */
  CLI_ACTION_RUN,

  /** Reject the command line. `error_message` explains the failure. */
  CLI_ACTION_ERROR,
};

/** Parsed command line for one cwrap invocation. */
struct CliOptions {
  /** Selected action after applying flag precedence. */
  enum CliAction action;

  /** Wrapping column. Defaults to `WRAP_COLUMN_DEFAULT` when `--width` is omitted. */
  ptrdiff_t width;

  /** Whether input comes from `stdin` instead of from `paths`. */
  bool is_stdin;

  /**
   * File input paths, pointing into the reordered `argv` passed to `cli_parse`. Every item is
   * non-`NULL`. Valid while the `argv` passed to `cli_parse` is. `NULL` in standard-input mode.
   */
  char** paths;

  /** Number of paths in `paths`. Zero in standard-input mode and positive otherwise. */
  ptrdiff_t path_count;

  /** Whether to rewrite each input file in place. */
  bool is_in_place;

  /** Whether to report files that would change without writing them. */
  bool is_check;

  /**
   * Failure diagnostic, non-empty exactly when `action` is `CLI_ACTION_ERROR`. It is a formatted
   * copy in the arena `cli_parse` was given, so it holds no pointer into `argv` or into copt's
   * scratch storage.
   */
  struct Str error_message;
};

/**
 * @brief Parses `argv` into `options`, resolving the action by fixed precedence.
 *
 * Never prints or exits. The caller inspects `options->action` and acts on it. On a rejected
 * command line, `options->action` is `CLI_ACTION_ERROR` and `options->error_message` holds the
 * first diagnostic. May permute `argv[1..argc-1]` in place (it reorders options ahead of positional
 * arguments). It leaves `argv[0]` untouched.
 *
 * `paths` aliases the positional tail of the reordered `argv`, so the result stays valid only while
 * that argument vector does. `error_message` is a formatted copy in `arena`, so the diagnostic does
 * not retain a pointer into copt's scratch storage. A short option's spelling lives in the parser
 * that `cli_parse` keeps on its own stack, so a later change to avoid the copy would hand back a
 * pointer that dangles as soon as `cli_parse` returns.
 *
 * @param options Receives the fully resolved parse result. Must not be `NULL`.
 * @param argc    Argument count.
 * @param argv    Argument vector. Elements after `argv[0]` may be reordered. Must not be `NULL`.
 * @param arena   Arena that owns `error_message`. Must not be `NULL`.
 */
void cli_parse(struct CliOptions* options, int argc, char** argv, struct Arena* arena)
    __attribute__((nonnull(1, 3, 4)));

/**
 * @brief Writes the `cwrap <version>` line to standard output.
 *
 * The line is formatted whole and printed in one `os_print` call.
 *
 * @param os      Host to print on. Must not be `NULL`.
 * @param scratch Arena the line is formatted in.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_version(struct Os* os, struct Arena scratch) __attribute__((nonnull(1)));

/**
 * @brief Writes usage and option help to standard output.
 *
 * The text is formatted whole and printed in one `os_print` call.
 *
 * @param program_name Program name to show in usage lines, such as `argv[0]`.
 * @param os           Host to print on. Must not be `NULL`.
 * @param scratch      Arena the text is formatted in. The program name is bounded only by
 *                     `ARG_MAX`, so it must hold the text around it and the name itself.
 * @return `0` on success, or `-1` when the write fails.
 */
int cli_print_usage(struct Str program_name, struct Os* os, struct Arena scratch)
    __attribute__((nonnull(2)));

#endif
