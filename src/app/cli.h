#ifndef CWRAP_CLI_H
#define CWRAP_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "core/error.h"

/** Largest wrapping column `--width` accepts. */
enum { WRAP_COLUMN_MAX = 10000 };

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
  size_t width;

  /** Whether input comes from `stdin` instead of from `paths`. */
  bool is_stdin;

  /**
   * File input paths, pointing into the reordered `argv` passed to `cli_parse`. Every item is
   * non-`NULL`. Valid until the caller returns from `cli_dispatch`. `NULL` in standard-input mode.
   */
  char** paths;

  /** Number of paths in `paths`. Zero in standard-input mode and positive otherwise. */
  int path_count;

  /** Whether to rewrite each input file in place. */
  bool is_in_place;

  /** Whether to report files that would change without writing them. */
  bool is_check;

  /** Failure diagnostic, non-empty when `action` is `CLI_ACTION_ERROR`. */
  char error_message[ERROR_MESSAGE_SIZE];
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
 * that argument vector does. `error_message` is a fixed inline buffer holding a formatted copy, so
 * the diagnostic itself does not retain a pointer into copt's scratch storage.
 *
 * @param options Receives the fully resolved parse result. Must not be `NULL`.
 * @param argc    Argument count.
 * @param argv    Argument vector. Elements after `argv[0]` may be reordered. Must not be `NULL`.
 */
void cli_parse(struct CliOptions* options, int argc, char** argv) __attribute__((nonnull(1, 3)));

/**
 * @brief Writes the `cwrap <version>` line to `stream`.
 *
 * @param stream Destination stream. Must not be `NULL`.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set by the failing
 *         write, or to `EIO` when the stream had latched an error earlier and the original `errno`
 *         is no longer available, so the caller always has a reason to report.
 */
int cli_print_version(FILE* stream) __attribute__((nonnull(1)));

/**
 * @brief Writes usage and option help to `stream`.
 *
 * @param stream       Destination stream. Must not be `NULL`.
 * @param program_name Program name to show in usage lines. Must not be `NULL`.
 * @return `0` on success, or `-1` if writing to `stream` failed, with `errno` set as for
 *         `cli_print_version`.
 */
int cli_print_usage(FILE* stream, const char* program_name) __attribute__((nonnull(1, 2)));

#endif
