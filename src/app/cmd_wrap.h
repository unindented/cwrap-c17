#ifndef CWRAP_CMD_WRAP_H
#define CWRAP_CMD_WRAP_H

#include <stdbool.h>
#include <stddef.h>

#include "app/exit_code.h"

/** Caller-selected knobs for one wrap invocation. */
struct WrapOptions {
  /** Wrapping column. Must be at least 1. */
  size_t width;

  /** Whether to read the single source from `stdin` instead of from `paths`. Mutually exclusive. */
  bool is_stdin;

  /** File input paths. Every item must be non-`NULL`. Must be `NULL` in standard-input mode. */
  char* const* paths;

  /** Number of paths in `paths`. Must be zero in standard-input mode and positive otherwise. */
  int path_count;

  /** Whether to rewrite each input file in place. */
  bool is_in_place;

  /** Whether to report files that would change without writing them. */
  bool is_check;
};

/**
 * @brief Runs the wrap command: reads, rewrites, and emits the selected inputs.
 *
 * Input is either the paths array or `stdin`; the two modes are mutually exclusive. In the default
 * mode, prints each rewritten input to `stdout`. In in-place mode, overwrites each changed file. In
 * check mode, emits no rewritten source and reports every input that would change. On failure,
 * prints the collected diagnostic to `stderr`.
 *
 * @param options Wrap knobs such as width and write mode. Must not be `NULL`.
 * @return `EXIT_CODE_OK` when every input is already wrapped or was emitted successfully, or
 *         `EXIT_CODE_FAILURE` when an input cannot be read, rewritten, or emitted, or when check
 *         mode finds an input that would change.
 */
enum ExitCode cmd_wrap_run(const struct WrapOptions* options) __attribute__((nonnull(1)));

#endif
