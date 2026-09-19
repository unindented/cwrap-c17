#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/wrap_input.h"
#include "core/error.h"
#include "runtime/fs.h"
#include "shared/string_buffer.h"

/**
 * @brief Creates a temporary source fixture.
 *
 * @param file_path Writable `mkstemp` template. Receives the created path. Must not be `NULL`.
 * @param source    Terminated source text to write. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int init_source_fixture(char file_path[static 1], const char* source) {
  const int fd = mkstemp(file_path);
  TEST_ASSERT(fd >= 0);
  if (fd < 0) {
    return -1;
  }
  const int close_rc = close(fd);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    (void)unlink(file_path);
    return -1;
  }
  const int rc = fs_write_file(file_path, source, strlen(source), NULL, 0);
  TEST_CHECK(rc == 0);
  if (rc != 0) {
    (void)unlink(file_path);
  }
  return rc;
}

// Stable file input succeeds silently in check mode.
static void test_file_check_ok_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-input.XXXXXX";
  if (init_source_fixture(file_path, "// one two\n") != 0) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapInputResult result =
      wrap_input_process_file(file_path, 40, false, true, &error_buffer);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(error_buffer.len == 0);
  string_buffer_free(&error_buffer);
  (void)unlink(file_path);
}

// Stable stream input succeeds silently in check mode.
static void test_stream_check_ok_result(void) {
  char source[] = "// one two\n";
  FILE* stream = fmemopen(source, strlen(source), "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapInputResult result =
      wrap_input_process_stream(stream, "stdin", 40, true, &error_buffer);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(error_buffer.len == 0);
  // The processing contract does not close the borrowed stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
  string_buffer_free(&error_buffer);
}

// File check mode retains the distinct changed result and path attribution.
static void test_file_check_change_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-input.XXXXXX";
  if (init_source_fixture(file_path, "// one\n// two\n") != 0) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapInputResult result =
      wrap_input_process_file(file_path, 40, false, true, &error_buffer);

  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected), "would rewrap '%s'", file_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  string_buffer_free(&error_buffer);
  (void)unlink(file_path);
}

// Stream check mode uses the supplied display name when it reports a change.
static void test_stream_check_change_result(void) {
  char source[] = "// one\n// two\n";
  FILE* stream = fmemopen(source, strlen(source), "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapInputResult result =
      wrap_input_process_stream(stream, "stdin", 40, true, &error_buffer);

  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, "would rewrap 'stdin'") == 0);
  // The processing contract does not close the borrowed stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
  string_buffer_free(&error_buffer);
}

// A file read error remains distinct from a check-mode change and keeps the existing diagnostic.
static void test_file_read_error_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-input.XXXXXX";
  if (init_source_fixture(file_path, "") != 0) {
    return;
  }
  (void)unlink(file_path);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  const enum WrapInputResult result =
      wrap_input_process_file(file_path, 40, false, true, &error_buffer);

  char system_message[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read file: %s ('%s')",
               error_system_message(system_message, sizeof(system_message), ENOENT), file_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  string_buffer_free(&error_buffer);
}

// A stream read failure is attributed to the stream name and remains an operational error.
static void test_stream_read_error_result(void) {
  FILE* stream = fopen("/dev/null", "w");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapInputResult result =
      wrap_input_process_stream(stream, "stdin", 40, true, &error_buffer);

  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read input: %s (from 'stdin')",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);
  clearerr(stream);
  // The processing contract does not close the borrowed stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
  string_buffer_free(&error_buffer);
}

TEST_LIST = {
    {"file check ok result", test_file_check_ok_result},
    {"stream check ok result", test_stream_check_ok_result},
    {"file check change result", test_file_check_change_result},
    {"stream check change result", test_stream_check_change_result},
    {"file read error result", test_file_read_error_result},
    {"stream read error result", test_stream_read_error_result},
    {NULL, NULL},
};
