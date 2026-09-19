#ifndef CWRAP_WRAP_INPUT_H
#define CWRAP_WRAP_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct StringBuffer;

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
 * @brief Reads, rewrites, and emits one source file.
 *
 * @param file_path    Input path and in-place output path. Must not be `NULL`.
 * @param width        Wrapping column. Must be at least 1.
 * @param is_in_place  Whether to overwrite a changed source.
 * @param is_check     Whether to report a change without emitting it.
 * @param error_buffer Growable buffer that receives diagnostics. Must be initialized. Must not be
 *                     `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` when check mode finds a
 *         change, or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
enum WrapInputResult wrap_input_process_file(const char* file_path,
                                             size_t width,
                                             bool is_in_place,
                                             bool is_check,
                                             struct StringBuffer* error_buffer)
    __attribute__((nonnull(1, 5)));

/**
 * @brief Reads, rewrites, and emits one source stream.
 *
 * The stream is read to EOF but is not closed. Stream input cannot be rewritten in place.
 *
 * @param stream       Source stream. Must not be `NULL`.
 * @param input_name   Name used to attribute diagnostics, such as `stdin`. Must not be `NULL`.
 * @param width        Wrapping column. Must be at least 1.
 * @param is_check     Whether to report a change without emitting it.
 * @param error_buffer Growable buffer that receives diagnostics. Must be initialized. Must not be
 *                     `NULL`.
 * @return The processing outcome, as for `wrap_input_process_file`.
 */
enum WrapInputResult wrap_input_process_stream(FILE* stream,
                                               const char* input_name,
                                               size_t width,
                                               bool is_check,
                                               struct StringBuffer* error_buffer)
    __attribute__((nonnull(1, 2, 5)));

#endif
