#include "domain/wrap_file.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "domain/rewrite.h"
#include "runtime/fs.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

/**
 * @brief Emits one rewritten source according to the selected output mode.
 *
 * @param error_buffer     Diagnostic buffer. Must not be `NULL`.
 * @param file_path        Input path and in-place output path. Must not be `NULL`.
 * @param source           Original source bytes. Must not be `NULL`.
 * @param source_len       Length of `source` in bytes.
 * @param rewritten_source Rewritten source to emit. Must not be `NULL`.
 * @param is_in_place      Whether to overwrite a changed source.
 * @param is_check         Whether to report a change without emitting it.
 * @return `WRAP_FILE_RESULT_OK` on success, `WRAP_FILE_RESULT_CHANGED` when check mode finds a
 *         change, or `WRAP_FILE_RESULT_ERROR` on failure.
 */
static enum WrapFileResult wrap_file_process_emit(struct StringBuffer* error_buffer,
                                                  const char* file_path,
                                                  const char* source,
                                                  size_t source_len,
                                                  const struct StringBuffer* rewritten_source,
                                                  bool is_in_place,
                                                  bool is_check)
    __attribute__((nonnull(1, 2, 3, 5)));

/**
 * @brief Appends a formatted diagnostic to `error_buffer`.
 *
 * @param error_buffer Destination buffer. Must not be `NULL`.
 * @param fmt          `printf`-style format string. Must not be `NULL`.
 * @param ...          Arguments for `fmt`.
 * @return `0` when the diagnostic was appended, or `-1` on allocation failure.
 */
static int append_error(struct StringBuffer* error_buffer, const char* fmt, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

enum WrapFileResult wrap_file_process(struct StringBuffer* error_buffer,
                                      const char* file_path,
                                      size_t width,
                                      bool is_in_place,
                                      bool is_check) {
  char* source = NULL;
  size_t source_len = 0;
  char reason[FS_REASON_SIZE];
  if (fs_read_file(file_path, &source, &source_len, reason, sizeof(reason)) != 0) {
    (void)append_error(error_buffer, "failed to read file: %s ('%s')", reason, file_path);
    return WRAP_FILE_RESULT_ERROR;
  }

  struct Arena arena;
  arena_init(&arena);
  struct StringBuffer rewritten_source;
  string_buffer_init(&rewritten_source);
  char err[ERROR_MESSAGE_SIZE];
  enum WrapFileResult result = WRAP_FILE_RESULT_ERROR;
  if (rewrite_source(&rewritten_source, source, source_len, width, &arena, err, sizeof(err)) != 0) {
    (void)append_error(error_buffer, "failed to rewrap file: %s ('%s')", err, file_path);
    goto cleanup;
  }
  result = wrap_file_process_emit(error_buffer, file_path, source, source_len, &rewritten_source,
                                  is_in_place, is_check);

cleanup:
  string_buffer_free(&rewritten_source);
  arena_free(&arena);
  free(source);
  return result;
}

static enum WrapFileResult wrap_file_process_emit(struct StringBuffer* error_buffer,
                                                  const char* file_path,
                                                  const char* source,
                                                  size_t source_len,
                                                  const struct StringBuffer* rewritten_source,
                                                  bool is_in_place,
                                                  bool is_check) {
  const bool is_changed =
      rewritten_source->len != source_len ||
      memcmp(rewritten_source->data == NULL ? "" : rewritten_source->data, source, source_len) != 0;
  if (is_check) {
    if (!is_changed) {
      return WRAP_FILE_RESULT_OK;
    }
    (void)append_error(error_buffer, "would rewrap '%s'", file_path);
    return WRAP_FILE_RESULT_CHANGED;
  }

  if (is_in_place) {
    char reason[FS_REASON_SIZE];
    if (is_changed &&
        fs_write_file(file_path, rewritten_source->data == NULL ? "" : rewritten_source->data,
                      rewritten_source->len, reason, sizeof(reason)) != 0) {
      (void)append_error(error_buffer, "failed to write file: %s ('%s')", reason, file_path);
      return WRAP_FILE_RESULT_ERROR;
    }
    return WRAP_FILE_RESULT_OK;
  }

  const size_t written = fwrite(rewritten_source->data == NULL ? "" : rewritten_source->data, 1,
                                rewritten_source->len, stdout);
  const int write_errno = errno;
  if (ferror(stdout) != 0) {
    char message[ERROR_MESSAGE_SIZE];
    const int error_number = write_errno == 0 ? EIO : write_errno;
    (void)append_error(error_buffer, "failed to write rewritten file to 'stdout': %s",
                       error_system_message(message, sizeof(message), error_number));
    return WRAP_FILE_RESULT_ERROR;
  }
  if (written != rewritten_source->len) {
    (void)append_error(error_buffer,
                       "failed to write rewritten file to 'stdout': wrote only %zu of %zu bytes",
                       written, rewritten_source->len);
    return WRAP_FILE_RESULT_ERROR;
  }
  return WRAP_FILE_RESULT_OK;
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
