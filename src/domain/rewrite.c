#include "domain/rewrite.h"

#include <assert.h>

#include "domain/comment.h"
#include "domain/doxygen.h"
#include "domain/fill.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

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
    cursor = block->end;
  }

  buf_append(buffer, str_tail(source, cursor));
}
