#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/error.h"
#include "core/string_buffer.h"
#include "domain/wrap_file.h"
#include "runtime/fs.h"

/**
 * @brief Creates a temporary source fixture.
 *
 * @param file_path Writable `mkstemp` template. Receives the created path.
 * @param source    Terminated source text to write.
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

// Check mode reports a distinct changed result and records the affected path.
static void test_check_change_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-file.XXXXXX";
  if (init_source_fixture(file_path, "// one\n// two\n") != 0) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapFileResult result = wrap_file_process(&error_buffer, file_path, 40, false, true);

  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len = snprintf(expected, sizeof(expected), "would rewrap '%s'", file_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(result == WRAP_FILE_RESULT_CHANGED);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  string_buffer_free(&error_buffer);
  (void)unlink(file_path);
}

// Check mode reports success without a diagnostic when the source is already stable.
static void test_check_ok_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-file.XXXXXX";
  if (init_source_fixture(file_path, "// one two\n") != 0) {
    return;
  }
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  const enum WrapFileResult result = wrap_file_process(&error_buffer, file_path, 40, false, true);

  TEST_CHECK(result == WRAP_FILE_RESULT_OK);
  TEST_CHECK(error_buffer.len == 0);

  string_buffer_free(&error_buffer);
  (void)unlink(file_path);
}

// A read failure remains distinct from a check-mode change and carries the system reason.
static void test_read_error_result(void) {
  char file_path[] = "/tmp/cwrap-wrap-file.XXXXXX";
  if (init_source_fixture(file_path, "") != 0) {
    return;
  }
  (void)unlink(file_path);

  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);
  const enum WrapFileResult result = wrap_file_process(&error_buffer, file_path, 40, false, true);

  char system_message[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read file: %s ('%s')",
               error_system_message(system_message, sizeof(system_message), ENOENT), file_path);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(result == WRAP_FILE_RESULT_ERROR);
  TEST_CHECK(error_buffer.data != NULL && strcmp(error_buffer.data, expected) == 0);

  string_buffer_free(&error_buffer);
}

TEST_LIST = {
    {"check change result", test_check_change_result},
    {"check ok result", test_check_ok_result},
    {"read error result", test_read_error_result},
    {NULL, NULL},
};
