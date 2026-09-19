#ifndef CWRAP_STREAM_H
#define CWRAP_STREAM_H

#include <stddef.h>
#include <stdio.h>

/** Size in bytes of a stream failure reason, including the `NUL` terminator. */
enum { STREAM_REASON_SIZE = 256 };

/**
 * @brief Reads a stream to EOF into a freshly allocated, `NUL`-terminated buffer.
 *
 * The stream need not be seekable and is not closed. On success the caller owns `*data_out` and
 * must `free` it. The function writes both output parameters only on success and rejects embedded
 * `NUL` bytes, preserving the text invariant established by `fs_read_file` for regular files.
 *
 * @param stream       Stream to read. Must not be `NULL`.
 * @param data_out     Receives the allocated buffer. Must not be `NULL`.
 * @param data_len_out Receives the byte count excluding the terminator. Must not be `NULL`.
 * @param reason       Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` on a read, allocation, size, or embedded-`NUL` failure.
 */
int stream_read_all(FILE* stream,
                    char** data_out,
                    size_t* data_len_out,
                    char* reason,
                    size_t reason_len) __attribute__((nonnull(1, 2, 3)));

#endif
