#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "runtime/stream.h"

// A non-seekable-style read contract returns every byte and a terminator without closing input.
static void test_read_all_returns_stream_contents(void) {
  char source[] = "// one\n// two\n";
  FILE* stream = fmemopen(source, strlen(source), "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }

  char* data = NULL;
  size_t data_len = 0;
  char reason[STREAM_REASON_SIZE] = "untouched";
  TEST_CHECK(stream_read_all(stream, &data, &data_len, reason, sizeof(reason)) == 0);
  TEST_ASSERT(data != NULL);
  if (data != NULL) {
    TEST_CHECK(data_len == strlen(source));
    TEST_CHECK(memcmp(data, source, data_len) == 0);
    TEST_CHECK(data[data_len] == '\0');
    TEST_CHECK(strcmp(reason, "untouched") == 0);
    free(data);
  }
  // `stream_read_all` borrows rather than closes the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
}

// Empty input still returns an owned, terminated buffer.
static void test_read_all_accepts_empty_stream(void) {
  FILE* stream = tmpfile();
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }

  char* data = NULL;
  size_t data_len = 99;
  TEST_CHECK(stream_read_all(stream, &data, &data_len, NULL, 0) == 0);
  TEST_ASSERT(data != NULL);
  if (data != NULL) {
    TEST_CHECK(data_len == 0);
    TEST_CHECK(strcmp(data, "") == 0);
    free(data);
  }
  // `stream_read_all` borrows rather than closes the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
}

// Input larger than one read chunk is accumulated without truncation.
static void test_read_all_accepts_multiple_chunks(void) {
  enum { SOURCE_LEN = 20000 };
  char* source = malloc(SOURCE_LEN);
  TEST_ASSERT(source != NULL);
  if (source == NULL) {
    return;
  }
  memset(source, 'x', SOURCE_LEN);
  FILE* stream = fmemopen(source, SOURCE_LEN, "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    free(source);
    return;
  }

  char* data = NULL;
  size_t data_len = 0;
  TEST_CHECK(stream_read_all(stream, &data, &data_len, NULL, 0) == 0);
  TEST_ASSERT(data != NULL);
  if (data != NULL) {
    TEST_CHECK(data_len == SOURCE_LEN);
    TEST_CHECK(memcmp(data, source, SOURCE_LEN) == 0);
    free(data);
  }
  // `stream_read_all` borrows rather than closes the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
  free(source);
}

// An embedded NUL is rejected and leaves output parameters untouched.
static void test_read_all_rejects_embedded_nul(void) {
  char source[] = {'a', '\0', 'b'};
  FILE* stream = fmemopen(source, sizeof(source), "r");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  char sentinel[] = "unchanged";
  char* data = sentinel;
  size_t data_len = 99;
  char reason[STREAM_REASON_SIZE] = "";

  TEST_CHECK(stream_read_all(stream, &data, &data_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(data == sentinel);
  TEST_CHECK(data_len == 99);
  TEST_CHECK(strcmp(reason, "contains an embedded NUL byte") == 0);
  // `stream_read_all` borrows rather than closes the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
}

// A read error reports the stream's errno and leaves output parameters untouched.
static void test_read_all_reports_read_error(void) {
  FILE* stream = fopen("/dev/null", "w");
  TEST_ASSERT(stream != NULL);
  if (stream == NULL) {
    return;
  }
  char sentinel[] = "unchanged";
  char* data = sentinel;
  size_t data_len = 99;
  char reason[STREAM_REASON_SIZE] = "";

  TEST_CHECK(stream_read_all(stream, &data, &data_len, reason, sizeof(reason)) == -1);
  TEST_CHECK(data == sentinel);
  TEST_CHECK(data_len == 99);
  char expected[STREAM_REASON_SIZE];
  TEST_CHECK(strcmp(reason, error_system_message(expected, sizeof(expected), EBADF)) == 0);
  clearerr(stream);
  // `stream_read_all` borrows rather than closes the stream.
  // cppcheck-suppress deallocuse
  TEST_CHECK(fclose(stream) == 0);
}

TEST_LIST = {
    {"read all returns stream contents", test_read_all_returns_stream_contents},
    {"read all accepts empty stream", test_read_all_accepts_empty_stream},
    {"read all accepts multiple chunks", test_read_all_accepts_multiple_chunks},
    {"read all rejects embedded nul", test_read_all_rejects_embedded_nul},
    {"read all reports read error", test_read_all_reports_read_error},
    {NULL, NULL},
};
