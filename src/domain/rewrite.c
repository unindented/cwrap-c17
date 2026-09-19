#include "domain/rewrite.h"

#include "core/error.h"
#include "domain/comment.h"
#include "domain/doxygen.h"
#include "domain/fill.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

int rewrite_source(struct StringBuffer* buffer,
                   const char* source,
                   size_t source_len,
                   size_t width,
                   struct Arena* arena,
                   char* err,
                   size_t err_len) {
  if (width == 0) {
    return error_report(err, err_len, "wrapping column must be a positive integer");
  }

  const char* bytes = source == NULL ? "" : source;
  struct CommentSpanList spans;
  if (lex_comment_spans(bytes, source_len, arena, &spans, err, err_len) != 0) {
    return -1;
  }

  struct CommentBlockList blocks;
  if (comment_group(bytes, source_len, &spans, arena, &blocks, err, err_len) != 0) {
    return -1;
  }

  size_t cursor = 0;
  for (size_t i = 0; i < blocks.count; i++) {
    const struct CommentBlock* block = &blocks.items[i];
    if (block->start > cursor) {
      if (string_buffer_append_len(buffer, bytes + cursor, block->start - cursor) != 0) {
        return error_report(err, err_len, "out of memory");
      }
    }

    struct BodyLineList lines;
    if (comment_extract_body(bytes, block, arena, &lines, err, err_len) != 0) {
      return -1;
    }
    if (doxygen_align_parameters(&lines, arena, err, err_len) != 0) {
      return -1;
    }
    if (fill_body_lines(&lines, comment_first_prefix_columns(block), comment_prefix_columns(block),
                        width, block->shape == COMMENT_SHAPE_HANGING, arena, err, err_len) != 0) {
      return -1;
    }
    if (comment_emit(buffer, bytes, block, &lines) != 0) {
      return error_report(err, err_len, "out of memory");
    }
    cursor = block->end;
  }

  if (cursor < source_len) {
    if (string_buffer_append_len(buffer, bytes + cursor, source_len - cursor) != 0) {
      return error_report(err, err_len, "out of memory");
    }
  }
  return 0;
}
