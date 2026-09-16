#ifndef CWRAP_FILL_H
#define CWRAP_FILL_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct BodyLineList;

/**
 * @brief Refills wrapable runs in `lines` to `width` using STYLE.md atoms.
 *
 * Prose paragraphs are joined and greedily filled. A Doxygen tag line starts a new paragraph and
 * keeps the tag on its first line. A wrapped tag description indents to the description column.
 * List items, fenced or indented samples, and decorative lines are left untouched. When
 * `has_hanging_closer` is true, the last line of a paragraph reserves three columns for a trailing
 * closer.
 *
 * @param lines                Body lines to refill in place. Must not be `NULL`.
 * @param first_prefix_columns Display columns before the first body line.
 * @param prefix_columns       Display columns occupied by each continuation prefix.
 * @param width                Wrapping column, inclusive.
 * @param has_hanging_closer   Whether to reserve a trailing closer on the last hanging-indent
 *                              line.
 * @param arena                Arena that owns replacement line texts. Must not be `NULL`.
 * @param err                  Receives a diagnostic on failure. May be `NULL` only when
 *                              `err_len` is 0.
 * @param err_len              Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int fill_body_lines(struct BodyLineList* lines,
                    size_t first_prefix_columns,
                    size_t prefix_columns,
                    size_t width,
                    bool has_hanging_closer,
                    struct Arena* arena,
                    char* err,
                    size_t err_len) __attribute__((nonnull(1, 6)));

#endif
