#ifndef CWRAP_WRAP_INPUT_H
#define CWRAP_WRAP_INPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Log;
struct Os;

/**
 * Largest source `wrap_input_process_file` and `wrap_input_process_stdin` read, in bytes.
 *
 * A source is held whole while it is rewritten, and the rewrite holds several times that again: the
 * comment spans, the refilled blocks, and the rewritten output. Without a ceiling, the memory one
 * run needs is bounded only by the largest file it is pointed at.
 *
 * 64 MiB is deliberately unreachable by legitimate use rather than tuned to an optimum. It sits
 * several times above the largest C sources in common use, such as SQLite's single-file
 * amalgamation at about 9 MB, so no real input meets it. A pathological file fails with a
 * diagnostic from its size alone, before it is read, rather than exhausting memory. Standard input
 * has no size to check ahead of the read, so its read stops one byte past the limit.
 */
enum { SOURCE_LEN_MAX = 64 * 1024 * 1024 };

/**
 * Operation that leads the message of a file read that fails, as a string literal.
 */
#define WRAP_INPUT_READ_FILE_OPERATION "failed to read file"

/** Outcome of processing one source input. */
enum WrapInputResult {
  /** The source was emitted successfully or already satisfied check mode. */
  WRAP_INPUT_RESULT_OK,

  /** Check mode found that the source would change. */
  WRAP_INPUT_RESULT_CHANGED,

  /** Reading, rewriting, or emitting the source failed. */
  WRAP_INPUT_RESULT_ERROR,
};

/**
 * @brief Reads one source file into `perm`, the first phase of `wrap_input_process_file`.
 *
 * Before it reads, it names the phase and the file for the out-of-memory report. A pass that only
 * checks that every input can be read, before any is written, calls it on its own and drops the
 * bytes.
 *
 * @param file_path  Path of the file to read.
 * @param os         Host to read from. Must not be `NULL`.
 * @param perm       Arena that receives the source. Must not be `NULL`.
 * @param source_out Receives the source. Written only on success. Must not be `NULL`.
 * @param log        Log that receives the failure, as `failed to read file: <cause> ('<path>')`.
 *                   Its arena must not overlap `perm`. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file cannot be read.
 */
int wrap_input_read_file(struct Str file_path,
                         struct Os* os,
                         struct Arena* perm,
                         struct Str* source_out,
                         struct Log* log) __attribute__((nonnull(2, 3, 4, 5)));

/**
 * @brief Reads, rewrites, and emits one source file.
 *
 * Holds the source and the rewritten output in `input`, and the rewrite's temporaries in `scratch`.
 * Both pass by value, so the next input reuses their memory. Before each phase it names the phase
 * and the file for the out-of-memory report.
 *
 * @param file_path   Input path and in-place output path.
 * @param width       Wrapping column. Must be at least 1.
 * @param is_in_place Whether to overwrite a changed source.
 * @param is_check    Whether to report a change without emitting it.
 * @param os          Host the file is read from and written to, and the output printed on. Must not
 *                    be `NULL`.
 * @param input       Arena for the source and the rewritten output.
 * @param scratch     Arena for the rewrite's temporaries.
 * @param log         Log that receives every failure, and in check mode the change. Its arena must
 *                    not overlap `input` or `scratch`. Must not be `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` when check mode finds a
 *         change, or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
enum WrapInputResult wrap_input_process_file(struct Str file_path,
                                             ptrdiff_t width,
                                             bool is_in_place,
                                             bool is_check,
                                             struct Os* os,
                                             struct Arena input,
                                             struct Arena scratch,
                                             struct Log* log) __attribute__((nonnull(5, 8)));

/**
 * @brief Reads, rewrites, and emits standard input.
 *
 * Standard input is read to its end but is not closed. It cannot be rewritten in place, and its
 * diagnostics name it `stdin`. The arenas are used as in `wrap_input_process_file`.
 *
 * @param width    Wrapping column. Must be at least 1.
 * @param is_check Whether to report a change without emitting it.
 * @param os       Host to read from and print on. Must not be `NULL`.
 * @param input    Arena for the source and the rewritten output.
 * @param scratch  Arena for the rewrite's temporaries.
 * @param log      Log that receives every failure, and in check mode the change. Its arena must not
 *                 overlap `input` or `scratch`. Must not be `NULL`.
 * @return The processing outcome, as for `wrap_input_process_file`.
 */
enum WrapInputResult wrap_input_process_stdin(ptrdiff_t width,
                                              bool is_check,
                                              struct Os* os,
                                              struct Arena input,
                                              struct Arena scratch,
                                              struct Log* log) __attribute__((nonnull(3, 6)));

#endif
