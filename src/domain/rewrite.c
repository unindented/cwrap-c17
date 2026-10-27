#include "domain/rewrite.h"

#include <assert.h>
#include <stdbool.h>

#include "domain/atom.h"
#include "domain/comment.h"
#include "domain/doxygen.h"
#include "domain/fill.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

/**
 * @brief Reports whether `block` is a slash-star comment on one line, with no tab inside it, that
 *        fits `width`.
 *
 * Such a comment needs no refill, so it keeps its exact bytes, such as a label padded to line up
 * with its neighbors.
 *
 * @param source Source bytes referenced by `block`.
 * @param block  Block to check. Must not be `NULL`.
 * @param width  Wrapping column.
 * @return `true` when the block is a slash-star comment that holds no newline or tab and whose
 *         line, indent included, fits `width`.
 */
static bool is_fitting_one_line_block(struct Str source,
                                      const struct CommentBlock* block,
                                      ptrdiff_t width) __attribute__((nonnull(2)));

void rewrite_source(struct Buf* buffer, struct Str source, ptrdiff_t width, struct Arena scratch) {
  assert(width > 0);

  const struct CommentSpanList spans = lex_comment_spans(source, &scratch);
  const struct CommentBlockList blocks = comment_group(source, &spans, &scratch);

  ptrdiff_t cursor = 0;
  for (ptrdiff_t i = 0; i < blocks.count; i++) {
    const struct CommentBlock* block = &blocks.items[i];
    if (block->start > cursor) {
      buf_append(buffer, (struct Str){source.data + cursor, block->start - cursor});
    }
    cursor = block->end;
    if (is_fitting_one_line_block(source, block, width)) {
      buf_append(buffer, (struct Str){source.data + block->start, block->end - block->start});
      continue;
    }

    // Each block gets its own copy of the scratch arena, so its lines and atoms are reclaimed
    // before the next block starts.
    struct Arena block_scratch = scratch;
    struct BodyLineList lines = comment_extract_body(source, block, &block_scratch);
    doxygen_align_parameters(&lines, &block_scratch);
    const struct BodyLineList aligned = lines;
    fill_body_lines(&lines, comment_prefix_columns_first(block), comment_prefix_columns(block),
                    width, block->shape == COMMENT_SHAPE_HANGING, &block_scratch);
    // A starred body that collapses onto the opener line gains a trailing closer there, so it is
    // filled again with room for that closer.
    if (comment_has_opener_closer(block, &lines)) {
      lines = aligned;
      fill_body_lines(&lines, comment_prefix_columns_first(block), comment_prefix_columns(block),
                      width, true, &block_scratch);
    }
    comment_emit(buffer, source, block, &lines);
  }

  buf_append(buffer, str_tail(source, cursor));
}

static bool is_fitting_one_line_block(struct Str source,
                                      const struct CommentBlock* block,
                                      ptrdiff_t width) {
  const struct Str text = {source.data + block->start, block->end - block->start};
  // A tab's width depends on its column, which `atom_column_width` does not know, so a comment with
  // one is refilled.
  return block->shape != COMMENT_SHAPE_LINE && !str_cut(text, '\n').is_found &&
         !str_cut(text, '\t').is_found && block->indent_columns + atom_column_width(text) <= width;
}
