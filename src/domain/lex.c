#include "domain/lex.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/error.h"
#include "shared/arena.h"

/** Lexer states. A slash may start a comment; a quote or backslash in code must not. */
enum LexState {
  /** Ordinary C source. */
  LEX_STATE_CODE,
  /** A slash that may open a comment. */
  LEX_STATE_SLASH,
  /** A `//` comment. */
  LEX_STATE_LINE_COMMENT,
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
  /** A quoted header name in an `#include` directive. */
  LEX_STATE_HEADER_QUOTED,
  /** A possible line splice in a quoted header name. */
  LEX_STATE_HEADER_QUOTED_BACKSLASH,
  /** An angle-bracketed header name in an `#include` directive. */
  LEX_STATE_HEADER_ANGLE,
  /** A possible line splice in an angle-bracketed header name. */
  LEX_STATE_HEADER_ANGLE_BACKSLASH,
};

/** Display columns per tab stop when measuring source positions. */
enum { TAB_WIDTH = 8 };

/**
 * @brief Reports whether `line` beginning at `start` is a `#include` line.
 *
 * @param source Source bytes. Must not be `NULL`.
 * @param start  Offset of the first byte of the line.
 * @param end    Offset of the first byte after the line, exclusive.
 * @return `true` when the line is `#include` after optional spaces, `false` otherwise.
 */
static bool is_include_line(const char* source, size_t start, size_t end)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the display width of `c` at `column`.
 *
 * @param column Current display column on the line.
 * @param c      Byte to measure. Tabs expand to `TAB_WIDTH`.
 * @return Columns consumed by `c`.
 */
static size_t byte_columns(size_t column, unsigned char c);

/**
 * @brief Consumes the byte after a backslash when it belongs to an LF or CRLF line splice.
 *
 * A CR keeps the lexer in its backslash state until the following LF completes the splice. The LF
 * moves it to `state_after` and resets the physical display column.
 *
 * @param source      Source bytes. Must not be `NULL`.
 * @param source_len  Number of bytes in `source`.
 * @param index       Index of the byte after a backslash or a CR already accepted by this helper.
 * @param state_after State to enter after the splice.
 * @param state       Current lexer state; receives `state_after` at the LF. Must not be `NULL`.
 * @param column      Current display column; receives 0 at the LF. Must not be `NULL`.
 * @return `true` when the current byte belongs to the splice, or `false` otherwise.
 */
static bool consume_line_splice(const char* source,
                                size_t source_len,
                                size_t index,
                                enum LexState state_after,
                                enum LexState* state,
                                size_t* column) __attribute__((nonnull(1, 5, 6)));

/**
 * @brief Walks `source`, returning the span count and filling `items` when provided.
 *
 * @param items      Span array to fill, or `NULL` to count only.
 * @param source     Source bytes. Must not be `NULL`.
 * @param source_len Number of bytes in `source`.
 * @return Number of spans found.
 */
static size_t scan_spans(struct CommentSpan* items, const char* source, size_t source_len)
    __attribute__((nonnull(2)));

/**
 * @brief Records one span when `items` is non-`NULL`.
 *
 * @param items      Span array, or `NULL` when only counting.
 * @param span_index Index of the span being recorded.
 * @param span       Span to store.
 */
static void store_span(struct CommentSpan* items, size_t span_index, struct CommentSpan span);

int lex_comment_spans(const char* source,
                      size_t source_len,
                      struct Arena* arena,
                      struct CommentSpanList* spans_out,
                      char* err,
                      size_t err_len) {
  if (source_len > 0 && source == NULL) {
    return error_report(err, err_len, "source is NULL with non-zero length");
  }

  const char* bytes = source == NULL ? "" : source;
  const size_t count = scan_spans(NULL, bytes, source_len);

  struct CommentSpan* items = NULL;
  if (count > 0) {
    items = arena_calloc(arena, count, sizeof(*items));
    if (items == NULL) {
      return error_report(err, err_len, "out of memory");
    }
    (void)scan_spans(items, bytes, source_len);
  }

  spans_out->items = items;
  spans_out->count = count;
  return 0;
}

static bool is_include_line(const char* source, size_t start, size_t end) {
  size_t i = start;
  while (i < end && (source[i] == ' ' || source[i] == '\t')) {
    i++;
  }
  if (i >= end || source[i] != '#') {
    return false;
  }
  i++;
  while (i < end && (source[i] == ' ' || source[i] == '\t')) {
    i++;
  }
  static const char include_keyword[] = "include";
  const size_t include_len = sizeof(include_keyword) - 1;
  if (i + include_len > end) {
    return false;
  }
  if (memcmp(source + i, include_keyword, include_len) != 0) {
    return false;
  }
  const size_t after_keyword = i + include_len;
  return after_keyword == end || source[after_keyword] == ' ' || source[after_keyword] == '\t' ||
         source[after_keyword] == '"' || source[after_keyword] == '<';
}

static size_t byte_columns(size_t column, unsigned char c) {
  if (c == '\t') {
    return TAB_WIDTH - (column % TAB_WIDTH);
  }
  if (c < 0x20 || c == 0x7F) {
    return 0;
  }
  return 1;
}

static bool consume_line_splice(const char* source,
                                size_t source_len,
                                size_t index,
                                enum LexState state_after,
                                enum LexState* state,
                                size_t* column) {
  if (source[index] == '\r' && index + 1 < source_len && source[index + 1] == '\n') {
    return true;
  }
  if (source[index] == '\n') {
    *state = state_after;
    *column = 0;
    return true;
  }
  return false;
}

static size_t scan_spans(struct CommentSpan* items, const char* source, size_t source_len) {
  enum LexState state = LEX_STATE_CODE;
  size_t count = 0;
  size_t line_start = 0;
  size_t column = 0;
  bool has_line_code = false;
  bool is_include = is_include_line(source, 0, source_len);
  size_t comment_start = 0;
  size_t comment_column = 0;
  bool is_comment_trailing = false;
  enum CommentKind comment_kind = COMMENT_KIND_LINE;

  for (size_t i = 0; i < source_len; i++) {
    const unsigned char c = (unsigned char)source[i];
    const size_t next_column = column + byte_columns(column, c);

    switch (state) {
      case LEX_STATE_CODE:
        if (c == '\\') {
          state = LEX_STATE_CODE_BACKSLASH;
        } else if (c == '\'') {
          has_line_code = true;
          state = LEX_STATE_CHAR;
        } else if (c == '/') {
          state = LEX_STATE_SLASH;
        } else if (c == '"' && is_include) {
          has_line_code = true;
          state = LEX_STATE_HEADER_QUOTED;
        } else if (c == '<' && is_include) {
          has_line_code = true;
          state = LEX_STATE_HEADER_ANGLE;
        } else if (c == '"') {
          has_line_code = true;
          state = LEX_STATE_STRING;
        } else if (c == '\n') {
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
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
            line_start = i + 1;
            column = 0;
            has_line_code = false;
            is_include = is_include_line(source, line_start, source_len);
            continue;
          }
        }
        break;

      case LEX_STATE_LINE_COMMENT:
        if (c == '\n') {
          const size_t comment_end = i > comment_start && source[i - 1] == '\r' ? i - 1 : i;
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
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
          continue;
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
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_STRING_ESCAPE:
        if (consume_line_splice(source, source_len, i, LEX_STATE_STRING, &state, &column)) {
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
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_CHAR_ESCAPE:
        if (consume_line_splice(source, source_len, i, LEX_STATE_CHAR, &state, &column)) {
          continue;
        }
        state = LEX_STATE_CHAR;
        break;

      case LEX_STATE_CODE_BACKSLASH:
        if (consume_line_splice(source, source_len, i, LEX_STATE_CODE, &state, &column)) {
          continue;
        }
        has_line_code = true;
        state = LEX_STATE_CODE;
        break;

      case LEX_STATE_HEADER_QUOTED:
        if (c == '\\') {
          state = LEX_STATE_HEADER_QUOTED_BACKSLASH;
        } else if (c == '"') {
          state = LEX_STATE_CODE;
        } else if (c == '\n') {
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_HEADER_QUOTED_BACKSLASH:
        if (consume_line_splice(source, source_len, i, LEX_STATE_HEADER_QUOTED, &state, &column)) {
          continue;
        }
        state = c == '"' ? LEX_STATE_CODE : LEX_STATE_HEADER_QUOTED;
        break;

      case LEX_STATE_HEADER_ANGLE:
        if (c == '\\') {
          state = LEX_STATE_HEADER_ANGLE_BACKSLASH;
        } else if (c == '>') {
          state = LEX_STATE_CODE;
        } else if (c == '\n') {
          line_start = i + 1;
          column = 0;
          has_line_code = false;
          is_include = is_include_line(source, line_start, source_len);
          state = LEX_STATE_CODE;
          continue;
        }
        break;

      case LEX_STATE_HEADER_ANGLE_BACKSLASH:
        if (consume_line_splice(source, source_len, i, LEX_STATE_HEADER_ANGLE, &state, &column)) {
          continue;
        }
        state = c == '>' ? LEX_STATE_CODE : LEX_STATE_HEADER_ANGLE;
        break;
    }

    column = next_column;
  }

  if (state == LEX_STATE_LINE_COMMENT) {
    store_span(items, count,
               (struct CommentSpan){
                   .kind = comment_kind,
                   .start = comment_start,
                   .end = source_len,
                   .opener_column = comment_column,
                   .is_trailing = is_comment_trailing,
               });
    count++;
  }

  return count;
}

static void store_span(struct CommentSpan* items, size_t span_index, struct CommentSpan span) {
  if (items != NULL) {
    items[span_index] = span;
  }
}
