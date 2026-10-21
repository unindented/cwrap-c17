#ifndef CWRAP_CMD_WRAP_H
#define CWRAP_CMD_WRAP_H

#include <stdbool.h>
#include <stddef.h>

#include "app/exit_code.h"

struct Os;

/** Caller-selected knobs for one wrap invocation. */
struct WrapOptions {
  /** Wrapping column. Must be at least 1. */
  ptrdiff_t width;

  /**
   * Whether to read the single source from `stdin` instead of from `paths`. Mutually exclusive with
   * a non-empty `paths`.
   */
  bool is_stdin;

  /** File input paths. Every item must be non-`NULL`. Must be `NULL` in standard-input mode. */
  char* const* paths;

  /** Number of paths in `paths`. Must be zero in standard-input mode and positive otherwise. */
  ptrdiff_t path_count;

  /** Whether to rewrite each input file in place. */
  bool is_in_place;

  /** Whether to report files that would change without writing them. */
  bool is_check;
};

/**
 * @brief Runs the wrap command: reads, rewrites, and emits the selected inputs.
 *
 * In the default mode, prints each rewritten input to standard output. In in-place mode, overwrites
 * each changed file. In check mode, emits no rewritten source and reports every input that would
 * change.
 *
 * The default mode reads, rewrites and prints each input in turn, and stops at the first that
 * fails, after printing those before it. In-place mode writes nothing once any input is bad: a pass
 * without writes first reads every path and logs each one that cannot be read or holds a `NUL`, in
 * path order, and only when it logged no error do the inputs run and write. A failure that pass
 * could not see, such as a write that fails, still stops the run part-way. Check mode writes
 * nothing, and reports each input in path order as it runs.
 *
 * It reserves the run's memory once: the log at its full size, and an input and a scratch arena,
 * which halve together under an address-space limit. Each input's source and rewritten output go in
 * a copy of the input arena, and its temporaries in a copy of the scratch, so the next input reuses
 * their memory. An input that outgrows an arena ends in `os_oom`, which prints the log. Otherwise,
 * on failure, it prints the log to standard error, once. Everything goes through `os`.
 *
 * @param options Wrap knobs such as width and write mode. Must not be `NULL`.
 * @param os      Host the inputs are read from and written to, and the log is printed on. Must not
 *                be `NULL`.
 * @return `EXIT_CODE_OK` when every input is already wrapped or was emitted successfully, or
 *         `EXIT_CODE_FAILURE` when an input cannot be read or emitted, or check mode finds an input
 *         that would change.
 */
enum ExitCode cmd_wrap_run(const struct WrapOptions* options, struct Os* os)
    __attribute__((nonnull(1, 2)));

#endif
