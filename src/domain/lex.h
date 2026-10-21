#ifndef CWRAP_LEX_H
#define CWRAP_LEX_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;

/** Kind of C comment the lexer recorded. */
enum CommentKind {
  /** A `//` comment running to the end of its line. */
  COMMENT_KIND_LINE,

  /** A slash-star block comment, possibly spanning lines. */
  COMMENT_KIND_BLOCK,
};

/** One comment span in the source, as a half-open byte range. */
struct CommentSpan {
  /** Whether the span is a line comment or a block comment. */
  enum CommentKind kind;

  /** Byte offset of the opening `//` or slash-star marker. */
  ptrdiff_t start;

  /** Byte offset immediately after the last byte of the comment. A line comment ends at the newline
      (exclusive) or at the end of the file, and a backslash line splice carries it past a newline.
      A block comment ends after its closer. */
  ptrdiff_t end;

  /** Display column of the first `/` of the opener, measured from the start of its line. */
  ptrdiff_t opener_column;

  /** Whether non-whitespace code or the closer of another comment appears on the opener's line
      before the comment. Trailing comments are recorded so a rewriter can copy them unchanged. */
  bool is_trailing;
};

/** Arena-owned list of comment spans in source order. */
struct CommentSpanList {
  /** Span array, or `NULL` when `count` is 0. */
  struct CommentSpan* items;

  /** Number of spans in `items`. */
  ptrdiff_t count;
};

/**
 * @brief Collects every C comment span in `source` into an arena-owned list.
 *
 * Strings and character literals are not comments, including a URL such as `http://x//y` inside
 * quotes. A quoted `#include` path lexes as a string, and an angle-bracketed one as code, so a
 * slash-star inside it opens a comment. Adjacent one-line slash-star comments stay separate spans.
 *
 * @param source Source bytes.
 * @param arena  Arena that owns the span array. Must not be `NULL`.
 * @return The span list.
 */
struct CommentSpanList lex_comment_spans(struct Str source, struct Arena* arena)
    __attribute__((nonnull(2)));

/**
 * @brief Returns the display column after `c` when it starts at `column`.
 *
 * This is the one measure of source columns, so a span's `opener_column` and any later indent
 * measurement agree.
 *
 * @param column Display column where `c` starts, measured from the start of its line.
 * @param c      Byte to measure. A tab advances to the next multiple of 8, a C0 control or DEL
 *               takes no column, and any other byte takes one.
 * @return Display column after `c`.
 */
ptrdiff_t lex_advance_column(ptrdiff_t column, unsigned char c);

#endif
