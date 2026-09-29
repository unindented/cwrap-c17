#ifndef CWRAP_FILL_H
#define CWRAP_FILL_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct BodyLineList;

/**
 * @brief Refills wrapable runs in `lines` to `width` using STYLE.md atoms.
 *
 * Prose paragraphs are joined and greedily filled. A Doxygen tag line starts a new paragraph that
 * its continuation lines join, and it keeps the tag keyword and any documented name on its first
 * line. A wrapped tag description hangs under the first description word, so an aligned `@param` or
 * `@retval` continues at the shared description column and any other tag under the first word after
 * its keyword. A Doxygen command, list marker, or tool directive inside a paragraph travels with
 * the word before it, so no continuation line reads back as a tag, list item, or directive. List
 * items, fenced or indented samples, decorative lines, and directive lines are left untouched. When
 * `has_trailing_closer` is true and a paragraph ends the body, its last line reserves three columns
 * for the closer that trails it.
 *
 * @param lines                Body lines to refill in place. Must not be `NULL`.
 * @param first_prefix_columns Display columns before the first body line.
 * @param prefix_columns       Display columns occupied by each continuation prefix.
 * @param width                Wrapping column, inclusive.
 * @param has_trailing_closer  Whether to reserve a closer that trails the last body line.
 * @param arena                Arena that owns replacement line texts. Must not be `NULL`.
 * @param err                  Receives a diagnostic on failure. May be `NULL` only when `err_len`
 *                             is 0.
 * @param err_len              Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int fill_body_lines(struct BodyLineList* lines,
                    size_t first_prefix_columns,
                    size_t prefix_columns,
                    size_t width,
                    bool has_trailing_closer,
                    struct Arena* arena,
                    char* err,
                    size_t err_len) __attribute__((nonnull(1, 6)));

#endif
