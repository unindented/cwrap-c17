#ifndef CWRAP_MEMORY_H
#define CWRAP_MEMORY_H

#include <stdbool.h>

#include "shared/str.h"

struct Log;
struct Os;

/**
 * @brief Names the log that the out-of-memory report prints first.
 *
 * A command sets its own log before its work starts and clears it once the log is printed. The
 * program has one thread, so this needs no lock.
 *
 * @param log The command's log, or `NULL` to clear it.
 */
void memory_oom_set_log(const struct Log* log);

/**
 * @brief Names what the program is doing, for the out-of-memory report.
 *
 * An input sets it before each phase it runs. A value stays until the next call replaces it.
 *
 * @param phase      Phase in progress, such as `rewrap`, or empty outside an input.
 * @param input_name Name of the input, a path or `stdin`. Ignored when `phase` is empty. Must stay
 *                   valid until the next call.
 * @param is_stream  Whether the input is a stream rather than a file.
 */
void memory_oom_set_job(struct Str phase, struct Str input_name, bool is_stream);

/**
 * @brief Prints the out-of-memory report to standard error.
 *
 * Prints the log `memory_oom_set_log` named, then a line in the shape of the program's other
 * diagnostics: `failed to <phase> file: out of memory ('<path>')`,
 * `failed to <phase> input: out of memory (from '<name>')` for a stream, or
 * `failed to run: out of memory` outside an input. It allocates nothing, so it works with every
 * arena full. The program's `os_oom` calls it.
 *
 * The inputs rewritten before the one that ran out keep their whole output, as after any other
 * failure, because an input prints its output in one `os_print` call once it is rewritten and
 * nothing is left buffered for `_Exit` to drop.
 *
 * @param os Host to print on. Must not be `NULL`.
 */
void memory_oom_report(struct Os* os) __attribute__((nonnull(1)));

#endif
