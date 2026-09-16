#ifndef CWRAP_WRAP_FILE_H
#define CWRAP_WRAP_FILE_H

#include <stdbool.h>
#include <stddef.h>

struct StringBuffer;

/** Outcome of processing one source file. */
enum WrapFileResult {
  /** The source was emitted successfully or already satisfied check mode. */
  WRAP_FILE_RESULT_OK,

  /** Check mode found that the source would change. */
  WRAP_FILE_RESULT_CHANGED,

  /** Reading, rewriting, or emitting the source failed. */
  WRAP_FILE_RESULT_ERROR,
};

/**
 * @brief Reads, rewrites, and emits one source file.
 *
 * In check mode, appends a diagnostic when the source would change. In in-place mode, overwrites a
 * changed source. Otherwise writes the rewritten source to `stdout` without flushing it.
 *
 * @param error_buffer Growable buffer that receives diagnostics. Must be initialized. Must not be
 *                     `NULL`.
 * @param file_path    Input path and in-place output path. Must not be `NULL`.
 * @param width        Wrapping column. Must be at least 1.
 * @param is_in_place  Whether to overwrite a changed source.
 * @param is_check     Whether to report a change without emitting it.
 * @return `WRAP_FILE_RESULT_OK` on success, `WRAP_FILE_RESULT_CHANGED` when check mode finds a
 *         change, or `WRAP_FILE_RESULT_ERROR` on failure.
 */
enum WrapFileResult wrap_file_process(struct StringBuffer* error_buffer,
                                      const char* file_path,
                                      size_t width,
                                      bool is_in_place,
                                      bool is_check) __attribute__((nonnull(1, 2)));

#endif
