#include "app/wrap_input.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "domain/rewrite.h"
#include "runtime/fs.h"
#include "runtime/stream.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

/** Storage kind of a source being processed. */
enum InputKind {
  /** Source read from a filesystem path. */
  INPUT_KIND_FILE,

  /** Source read from a stream. */
  INPUT_KIND_STREAM,
};

/**
 * @brief Rewrites and emits source bytes that have already been read.
 *
 * @param source       Original source bytes. Must not be `NULL`.
 * @param source_len   Length of `source` in bytes.
 * @param width        Wrapping column.
 * @param input_kind   Whether the source came from a file or stream.
 * @param input_name   Name used in diagnostics. Must not be `NULL`.
 * @param output_path  In-place destination, or `NULL` to emit to `stdout`.
 * @param is_check     Whether to report a change without emitting it.
 * @param error_buffer Diagnostic buffer. Must not be `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` for a check-mode change,
 *         or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
static enum WrapInputResult process_source(const char* source,
                                           size_t source_len,
                                           size_t width,
                                           enum InputKind input_kind,
                                           const char* input_name,
                                           const char* output_path,
                                           bool is_check,
                                           struct StringBuffer* error_buffer)
    __attribute__((nonnull(1, 5, 8)));

/**
 * @brief Emits one rewritten source according to the selected output mode.
 *
 * @param source           Original source bytes. Must not be `NULL`.
 * @param source_len       Length of `source` in bytes.
 * @param rewritten_source Rewritten source to emit. Must not be `NULL`.
 * @param input_kind       Whether the source came from a file or stream.
 * @param input_name       Name used in diagnostics. Must not be `NULL`.
 * @param output_path      In-place destination, or `NULL` to emit to `stdout`.
 * @param is_check         Whether to report a change without emitting it.
 * @param error_buffer     Diagnostic buffer. Must not be `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` for a check-mode change,
 *         or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
static enum WrapInputResult process_source_emit(const char* source,
                                                size_t source_len,
                                                const struct StringBuffer* rewritten_source,
                                                enum InputKind input_kind,
                                                const char* input_name,
                                                const char* output_path,
                                                bool is_check,
                                                struct StringBuffer* error_buffer)
    __attribute__((nonnull(1, 3, 5, 8)));

/**
 * @brief Appends a formatted diagnostic.
 *
 * @param error_buffer Destination buffer. Must not be `NULL`.
 * @param fmt          `printf`-style format string. Must not be `NULL`.
 * @param ...          Arguments for `fmt`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_error(struct StringBuffer* error_buffer, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

enum WrapInputResult wrap_input_process_file(const char* file_path,
                                             size_t width,
                                             bool is_in_place,
                                             bool is_check,
                                             struct StringBuffer* error_buffer) {
  char* source = NULL;
  size_t source_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(file_path, &source, &source_len, reason, sizeof(reason)) != 0) {
    (void)append_error(error_buffer, "failed to read file: %s ('%s')", reason, file_path);
    return WRAP_INPUT_RESULT_ERROR;
  }

  const char* output_path = is_in_place ? file_path : NULL;
  const enum WrapInputResult result = process_source(
      source, source_len, width, INPUT_KIND_FILE, file_path, output_path, is_check, error_buffer);
  free(source);
  return result;
}

enum WrapInputResult wrap_input_process_stream(FILE* stream,
                                               const char* input_name,
                                               size_t width,
                                               bool is_check,
                                               struct StringBuffer* error_buffer) {
  char* source = NULL;
  size_t source_len = 0;
  char reason[STREAM_REASON_SIZE];
  if (stream_read_all(stream, &source, &source_len, reason, sizeof(reason)) != 0) {
    (void)append_error(error_buffer, "failed to read input: %s (from '%s')", reason, input_name);
    return WRAP_INPUT_RESULT_ERROR;
  }

  const enum WrapInputResult result = process_source(source, source_len, width, INPUT_KIND_STREAM,
                                                     input_name, NULL, is_check, error_buffer);
  free(source);
  return result;
}

static enum WrapInputResult process_source(const char* source,
                                           size_t source_len,
                                           size_t width,
                                           enum InputKind input_kind,
                                           const char* input_name,
                                           const char* output_path,
                                           bool is_check,
                                           struct StringBuffer* error_buffer) {
  struct Arena arena;
  arena_init(&arena);
  struct StringBuffer rewritten_source;
  string_buffer_init(&rewritten_source);
  char err[ERROR_MESSAGE_SIZE];
  enum WrapInputResult result = WRAP_INPUT_RESULT_ERROR;

  if (rewrite_source(&rewritten_source, source, source_len, width, &arena, err, sizeof(err)) != 0) {
    if (input_kind == INPUT_KIND_FILE) {
      (void)append_error(error_buffer, "failed to rewrap file: %s ('%s')", err, input_name);
    } else {
      (void)append_error(error_buffer, "failed to rewrap input: %s (from '%s')", err, input_name);
    }
    goto cleanup;
  }
  result = process_source_emit(source, source_len, &rewritten_source, input_kind, input_name,
                               output_path, is_check, error_buffer);

cleanup:
  string_buffer_free(&rewritten_source);
  arena_free(&arena);
  return result;
}

static enum WrapInputResult process_source_emit(const char* source,
                                                size_t source_len,
                                                const struct StringBuffer* rewritten_source,
                                                enum InputKind input_kind,
                                                const char* input_name,
                                                const char* output_path,
                                                bool is_check,
                                                struct StringBuffer* error_buffer) {
  const bool is_changed =
      rewritten_source->len != source_len ||
      memcmp(rewritten_source->data == NULL ? "" : rewritten_source->data, source, source_len) != 0;
  if (is_check) {
    if (!is_changed) {
      return WRAP_INPUT_RESULT_OK;
    }
    (void)append_error(error_buffer, "would rewrap '%s'", input_name);
    return WRAP_INPUT_RESULT_CHANGED;
  }

  if (output_path != NULL) {
    char reason[FS_REASON_SIZE];
    if (is_changed &&
        fs_write_file(output_path, rewritten_source->data == NULL ? "" : rewritten_source->data,
                      rewritten_source->len, reason, sizeof(reason)) != 0) {
      (void)append_error(error_buffer, "failed to write file: %s ('%s')", reason, output_path);
      return WRAP_INPUT_RESULT_ERROR;
    }
    return WRAP_INPUT_RESULT_OK;
  }

  errno = 0;
  const size_t written = fwrite(rewritten_source->data == NULL ? "" : rewritten_source->data, 1,
                                rewritten_source->len, stdout);
  const int write_errno = errno;
  if (ferror(stdout) != 0) {
    char message[ERROR_MESSAGE_SIZE];
    const int error_number = write_errno == 0 ? EIO : write_errno;
    (void)append_error(error_buffer, "failed to write rewritten %s to 'stdout': %s",
                       input_kind == INPUT_KIND_FILE ? "file" : "input",
                       error_system_message(message, sizeof(message), error_number));
    return WRAP_INPUT_RESULT_ERROR;
  }
  if (written != rewritten_source->len) {
    (void)append_error(
        error_buffer, "failed to write rewritten %s to 'stdout': wrote only %zu of %zu bytes",
        input_kind == INPUT_KIND_FILE ? "file" : "input", written, rewritten_source->len);
    return WRAP_INPUT_RESULT_ERROR;
  }
  return WRAP_INPUT_RESULT_OK;
}

static int append_error(struct StringBuffer* error_buffer, const char* fmt, ...) {
  char message[ERROR_MESSAGE_SIZE];
  va_list ap;
  va_start(ap, fmt);
  error_report_va(message, sizeof(message), fmt, ap);
  va_end(ap);
  const size_t separator_len = error_buffer->len > 0 ? 1 : 0;
  if (string_buffer_reserve(error_buffer, separator_len + strlen(message)) != 0) {
    return -1;
  }
  if (separator_len > 0 && string_buffer_append_char(error_buffer, '\n') != 0) {
    return -1;
  }
  return string_buffer_append(error_buffer, message);
}
