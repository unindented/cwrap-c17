#ifndef CWRAP_REWRITE_H
#define CWRAP_REWRITE_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct Buf;

/**
 * @brief Rewrites wrappable comments in `source` into `buffer`, leaving code unchanged.
 *
 * Trailing comments after code are copied as-is. Line-comment runs and slash-star blocks are
 * refilled to `width` using STYLE.md atoms, Doxygen alignment, and the block's closer convention.
 *
 * The comment spans and blocks of the file live in `scratch` for the whole call, and each block
 * then works in its own copy of what remains, so one block's lines and atoms are reclaimed before
 * the next. `buffer` grows while those copies allocate, so its arena must not be `scratch`'s
 * region: the copies would overwrite the bytes it appended.
 *
 * @param buffer  Buffer that receives the rewritten file. Must not be `NULL`.
 * @param source  Source bytes.
 * @param width   Wrapping column. Must be at least 1.
 * @param scratch Arena for the spans, blocks, line texts and atoms, released when the call returns.
 */
void rewrite_source(struct Buf* buffer, struct Str source, ptrdiff_t width, struct Arena scratch)
    __attribute__((nonnull(1)));

#endif
