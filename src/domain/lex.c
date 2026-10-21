#include "domain/lex.h"

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

/** Lexer states. A slash may start a comment; a quote or backslash in code must not. */
enum LexState {
  /** Ordinary C source. */
  LEX_STATE_CODE,
  /** A slash that may open a comment. */
  LEX_STATE_SLASH,
  /** A `//` comment. */
  LEX_STATE_LINE_COMMENT,
  /** A possible line splice in a `//` comment. */
  LEX_STATE_LINE_COMMENT_BACKSLASH,
  /** The body of a slash-star comment. */
  LEX_STATE_BLOCK_COMMENT,
  /** A star that may close a slash-star comment. */
  LEX_STATE_BLOCK_STAR,
  /** A string literal. */
  LEX_STATE_STRING,
  /** An escaped byte in a string literal. */
  LEX_STATE_STRING_ESCAPE,
  /** A character literal. */
  LEX_STATE_CHAR,
  /** An escaped byte in a character literal. */
  LEX_STATE_CHAR_ESCAPE,
  /** A backslash in ordinary source. */
  LEX_STATE_CODE_BACKSLASH,
};

/**
 * Display columns per tab stop when measuring source positions. A power of two, so the column
 * within a stop is a mask: `%` on a signed column costs several instructions on every byte.
 */
enum { TAB_WIDTH = 8 };

_Static_assert((TAB_WIDTH & (TAB_WIDTH - 1)) == 0, "a tab stop is masked, so it is a power of two");

/**
 * @brief Consumes the byte after a backslash when it belongs to an LF or CRLF line splice.
 *
 * A CR keeps the lexer in its backslash state until the following LF completes the splice. The LF
 * moves it to `state_after` and resets the physical display column.
 *
 * @param state       Current lexer state; receives `state_after` at the LF. Must not be `NULL`.
 * @param column      Current display column; receives 0 at the LF. Must not be `NULL`.
 * @param source      Source bytes.
 * @param index       Index of the byte after a backslash or a CR already accepted by this helper.
 * @param state_after State to enter after the splice.
 * @return `true` when the current byte belongs to the splice, or `false` otherwise.
 */
static bool consume_line_splice(enum LexState* state,
                                ptrdiff_t* column,
                                struct Str source,
                                ptrdiff_t index,
                                enum LexState state_after) __attribute__((nonnull(1, 2)));

/**
 * @brief Walks `source`, returning the span count and filling `items` when provided.
 *
 * @param items  Span array to fill, or `NULL` to count only.
 * @param source Source bytes.
 * @return Number of spans found.
 */
static ptrdiff_t scan_spans(struct CommentSpan* items, struct Str source);

/**
 * @brief Records one span when `items` is non-`NULL`.
 *
 * @param items      Span array, or `NULL` when only counting.
 * @param span_index Index of the span being recorded.
 * @param span       Span to store.
 */
static void store_span(struct CommentSpan* items, ptrdiff_t span_index, struct CommentSpan span);

struct CommentSpanList lex_comment_spans(struct Str source, struct Arena* arena) {
  // Counting first sizes the array exactly. A bound from the source length alone would reserve tens
  // of bytes per source byte.
  const ptrdiff_t count = scan_spans(NULL, source);

  struct CommentSpan* items = NULL;
  if (count > 0) {
    items = arena_new(arena, count, struct CommentSpan);
    (void)scan_spans(items, source);
  }

  return (struct CommentSpanList){.items = items, .count = count};
}

ptrdiff_t lex_advance_column(ptrdiff_t column, unsigned char c) {
  if (c == '\t') {
    return column + TAB_WIDTH - (column & (TAB_WIDTH - 1));
  }
  if (c < 0x20 || c == 0x7F) {
    return column;
  }
  return column + 1;
}

static bool consume_line_splice(enum LexState* state,
                                ptrdiff_t* column,
                                struct Str source,
                                ptrdiff_t index,
                                enum LexState state_after) {
  if (source.data[index] == '\r' && index + 1 < source.len && source.data[index + 1] == '\n') {
    return true;
  }
  if (source.data[index] == '\n') {
    *state = state_after;
    *column = 0;
    return true;
  }
  return false;
}

static ptrdiff_t scan_spans(struct CommentSpan* items, struct Str source) {
  enum LexState state = LEX_STATE_CODE;
  ptrdiff_t count = 0;
  ptrdiff_t column = 0;
  bool has_line_code = false;
  ptrdiff_t comment_start = 0;
  ptrdiff_t comment_column = 0;
  bool is_comment_trailing = false;
  enum CommentKind comment_kind = COMMENT_KIND_LINE;

  for (ptrdiff_t i = 0; i < source.len; i++) {
    const unsigned char c = (unsigned char)source.data[i];
    const ptrdiff_t column_next = lex_advance_column(column, c);

    switch (state) {
      case LEX_STATE_CODE:
        if (c == '\\') {
          state = LEX_STATE_CODE_BACKSLASH;
        } else if (c == '\'') {
          has_line_code = true;
          state = LEX_STATE_CHAR;
        } else if (c == '/') {
          state = LEX_STATE_SLASH;
        } else if (c == '"') {
          has_line_code = true;
          state = LEX_STATE_STRING;
        } else if (c == '\n') {
          column = 0;
          has_line_code = false;
          continue;
        } else if (c != ' ' && c != '\t' && c != '\r') {
          has_line_code = true;
        }
        break;

      case LEX_STATE_SLASH:
        if (c == '/') {
          comment_start = i - 1;
          comment_column = column - 1;
          is_comment_trailing = has_line_code;
          comment_kind = COMMENT_KIND_LINE;
          state = LEX_STATE_LINE_COMMENT;
        } else if (c == '*') {
          comment_start = i - 1;
          comment_column = column - 1;
          is_comment_trailing = has_line_code;
          comment_kind = COMMENT_KIND_BLOCK;
          state = LEX_STATE_BLOCK_COMMENT;
        } else {
          has_line_code = true;
          state = LEX_STATE_CODE;
          if (c == '\n') {
            column = 0;
            has_line_code = false;
            continue;
          }
        }
        break;

      case LEX_STATE_LINE_COMMENT:
        if (c == '\\') {
          state = LEX_STATE_LINE_COMMENT_BACKSLASH;
        } else if (c == '\n') {
          const ptrdiff_t comment_end = i > comment_start && source.data[i - 1] == '\r' ? i - 1 : i;
          store_span(items, count,
                     (struct CommentSpan){
                         .kind = comment_kind,
                         .start = comment_start,
                         .end = comment_end,
                         .opener_column = comment_column,
                         .is_trailing = is_comment_trailing,
                     });
          count++;
          state = LEX_STATE_CODE;
          column = 0;
          has_line_code = false;
          continue;
        }
        break;

      case LEX_STATE_LINE_COMMENT_BACKSLASH:
        if (consume_line_splice(&state, &column, source, i, LEX_STATE_LINE_COMMENT)) {
          continue;
        }
        if (c != '\\') {
          state = LEX_STATE_LINE_COMMENT;
        }
        break;

      case LEX_STATE_BLOCK_COMMENT:
        if (c == '*') {
          state = LEX_STATE_BLOCK_STAR;
        } else if (c == '\n') {
          column = 0;
          continue;
        }
        break;

      case LEX_STATE_BLOCK_STAR:
        if (c == '/') {
          store_span(items, count,
                     (struct CommentSpan){
                         .kind = comment_kind,
                         .start = comment_start,
                         .end = i + 1,
                         .opener_column = comment_column,
                         .is_trailing = is_comment_trailing,
                     });
          count++;
          // A later comment on this line must not treat the one just closed as its indent.
          has_line_code = true;
          state = LEX_STATE_CODE;
        } else if (c == '*') {
          state = LEX_STATE_BLOCK_STAR;
        } else {
          state = LEX_STATE_BLOCK_COMMENT;
          if (c == '\n') {
            column = 0;
            continue;
          }
        }
        break;

      case LEX_STATE_STRING:
        if (c == '\\') {
          state = LEX_STATE_STRING_ESCAPE;
        } else if (c == '"') {
          state = LEX_STATE_CODE;
        } else if (c == '\n') {
          column = 0;
          has_line_code = false;
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_STRING_ESCAPE:
        if (consume_line_splice(&state, &column, source, i, LEX_STATE_STRING)) {
          continue;
        }
        state = LEX_STATE_STRING;
        break;

      case LEX_STATE_CHAR:
        if (c == '\\') {
          state = LEX_STATE_CHAR_ESCAPE;
        } else if (c == '\'') {
          state = LEX_STATE_CODE;
        } else if (c == '\n') {
          column = 0;
          has_line_code = false;
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_CHAR_ESCAPE:
        if (consume_line_splice(&state, &column, source, i, LEX_STATE_CHAR)) {
          continue;
        }
        state = LEX_STATE_CHAR;
        break;

      case LEX_STATE_CODE_BACKSLASH:
        if (consume_line_splice(&state, &column, source, i, LEX_STATE_CODE)) {
          continue;
        }
        has_line_code = true;
        state = LEX_STATE_CODE;
        break;
    }

    column = column_next;
  }

  if (state == LEX_STATE_LINE_COMMENT || state == LEX_STATE_LINE_COMMENT_BACKSLASH) {
    store_span(items, count,
               (struct CommentSpan){
                   .kind = comment_kind,
                   .start = comment_start,
                   .end = source.len,
                   .opener_column = comment_column,
                   .is_trailing = is_comment_trailing,
               });
    count++;
  }

  return count;
}

static void store_span(struct CommentSpan* items, ptrdiff_t span_index, struct CommentSpan span) {
  if (items != NULL) {
    items[span_index] = span;
  }
}
