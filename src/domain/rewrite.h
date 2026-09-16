#ifndef CWRAP_REWRITE_H
#define CWRAP_REWRITE_H

#include <stddef.h>

struct Arena;
struct StringBuffer;

/**
 * @brief Rewrites wrapable comments in `source` into `buffer`, leaving code unchanged.
 *
 * Trailing comments after code are copied as-is. Line-comment runs and slash-star blocks are
 * refilled to `width` using STYLE.md atoms, Doxygen alignment, and the block's closer convention.
 *
 * @param buffer     Buffer that receives the rewritten file. Must be initialized and not `NULL`.
 * @param source     Source bytes. Must hold at least `source_len` bytes. May be `NULL` only when
 *                   `source_len` is 0.
 * @param source_len Number of bytes in `source`.
 * @param width      Wrapping column. Must be at least 1.
 * @param arena      Arena that owns temporary spans, blocks, and line texts. Must not be `NULL`.
 * @param err        Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len    Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation or wrap failure.
 */
int rewrite_source(struct StringBuffer* buffer,
                   const char* source,
                   size_t source_len,
                   size_t width,
                   struct Arena* arena,
                   char* err,
                   size_t err_len) __attribute__((nonnull(1, 5)));

#endif
