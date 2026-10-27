#ifndef CWRAP_FILL_H
#define CWRAP_FILL_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct BodyLineList;

/**
 * @brief Refills wrappable runs in `lines` to `width` using the atoms of `atom_split`.
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
 * @param prefix_columns_first Display columns before the first body line.
 * @param prefix_columns       Display columns occupied by each continuation prefix.
 * @param width                Wrapping column, inclusive. Must be at least 1.
 * @param has_trailing_closer  Whether to reserve a closer that trails the last body line.
 * @param arena                Arena that owns replacement line texts. Must not be `NULL`.
 */
void fill_body_lines(struct BodyLineList* lines,
                     ptrdiff_t prefix_columns_first,
                     ptrdiff_t prefix_columns,
                     ptrdiff_t width,
                     bool has_trailing_closer,
                     struct Arena* arena) __attribute__((nonnull(1, 6)));

#endif
