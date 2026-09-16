#ifndef CWRAP_LEX_H
#define CWRAP_LEX_H

#include <stdbool.h>
#include <stddef.h>

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
  size_t start;

  /** Byte offset immediately after the last byte of the comment. A line comment ends at the newline
     (exclusive) or at the end of the file. A block comment ends after its closer. */
  size_t end;

  /** Display column of the first `/` of the opener, measured from the start of its line. */
  size_t opener_column;

  /** Whether non-whitespace code appears on the opener's line before the comment. Trailing comments
     are recorded so a rewriter can copy them unchanged. */
  bool is_trailing;
};

/** Arena-owned list of comment spans in source order. */
struct CommentSpanList {
  /** Span array, or `NULL` when `count` is 0. */
  struct CommentSpan* items;

  /** Number of spans in `items`. */
  size_t count;
};

/**
 * @brief Collects every C comment span in `source` into an arena-owned list.
 *
 * Strings and character literals are not comments, including a URL such as `http://x//y` inside
 * quotes. An `#include "..."` path is treated as code, not as a string. Adjacent one-line
 * slash-star comments stay separate spans.
 *
 * @param source     Source bytes. Must hold at least `source_len` bytes. May be `NULL` only when
 *                   `source_len` is 0.
 * @param source_len Number of bytes in `source`.
 * @param arena      Arena that owns the span array. Must not be `NULL`.
 * @param spans_out  Receives the span list. Written only on success. Must not be `NULL`.
 * @param err        Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len    Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int lex_comment_spans(const char* source,
                      size_t source_len,
                      struct Arena* arena,
                      struct CommentSpanList* spans_out,
                      char* err,
                      size_t err_len) __attribute__((nonnull(3, 4)));

#endif
