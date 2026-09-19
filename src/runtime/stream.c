#include "runtime/stream.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/error.h"
#include "shared/string_buffer.h"

/** Number of bytes requested from the stream at a time. */
enum { STREAM_READ_CHUNK_SIZE = 8192 };

/** Largest stream content length that leaves room for a `NUL` terminator. */
static const size_t STREAM_DATA_LEN_MAX = SIZE_MAX - 1;

int stream_read_all(FILE* stream,
                    char** data_out,
                    size_t* data_len_out,
                    char* reason,
                    size_t reason_len) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  int rc = -1;

  for (;;) {
    char chunk[STREAM_READ_CHUNK_SIZE];
    errno = 0;
    const size_t chunk_len = fread(chunk, 1, sizeof(chunk), stream);
    const int read_errno = errno;

    if (chunk_len > 0) {
      if (memchr(chunk, '\0', chunk_len) != NULL) {
        (void)error_report(reason, reason_len, "contains an embedded NUL byte");
        goto cleanup;
      }
      // One byte beyond the payload is always required for the terminator.
      if (chunk_len > STREAM_DATA_LEN_MAX - buffer.len) {
        (void)error_report(reason, reason_len, "exceeds max readable size (%zu bytes) at %zu bytes",
                           STREAM_DATA_LEN_MAX, SIZE_MAX);
        goto cleanup;
      }
      if (string_buffer_append_len(&buffer, chunk, chunk_len) != 0) {
        (void)error_report(reason, reason_len, "out of memory");
        goto cleanup;
      }
    }

    if (ferror(stream) != 0) {
      char message[STREAM_REASON_SIZE];
      const int error_number = read_errno == 0 ? EIO : read_errno;
      (void)error_report(reason, reason_len, "%s",
                         error_system_message(message, sizeof(message), error_number));
      goto cleanup;
    }
    if (chunk_len == sizeof(chunk)) {
      continue;
    }
    if (feof(stream) == 0) {
      (void)error_report(reason, reason_len, "read made no progress");
      goto cleanup;
    }

    const size_t data_len = buffer.len;
    char* data = string_buffer_steal(&buffer);
    if (data == NULL) {
      (void)error_report(reason, reason_len, "out of memory");
      goto cleanup;
    }
    *data_out = data;
    *data_len_out = data_len;
    rc = 0;
    break;
  }

cleanup:
  string_buffer_free(&buffer);
  return rc;
}
