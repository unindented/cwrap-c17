#include "domain/comment.h"

#include <stdbool.h>
#include <string.h>

#include "core/arena.h"
#include "core/ascii.h"
#include "core/error.h"
#include "core/string_buffer.h"
#include "domain/doxygen.h"
#include "domain/lex.h"

/** Display columns per tab stop when measuring source indentation. */
enum { TAB_WIDTH = 8 };

/**
 * @brief Reports whether `spans[index]` can extend the open `//` run ending at `previous_end`.
 *
 * @param source         Source bytes. Must not be `NULL`.
 * @param spans          Span list. Must not be `NULL`.
 * @param index          Candidate span index.
 * @param previous_end   Exclusive end of the previous span in the run.
 * @param indent_columns Required opener column.
 * @return `true` when the span continues the run.
 */
static bool is_line_run_continuation(const char* source,
                                     const struct CommentSpanList* spans,
                                     size_t index,
                                     size_t previous_end,
                                     size_t indent_columns) __attribute__((nonnull(1, 2)));

/**
 * @brief Classifies a slash-star span as starred or hanging from its continuation lines.
 *
 * @param source Source bytes. Must not be `NULL`.
 * @param span   Block-comment span. Must not be `NULL`.
 * @return `COMMENT_SHAPE_STARRED` when a continuation `*` sits at the decoration column
 *         (`opener_column + 1`), otherwise hanging. A `*` at the hanging-text column is a list
 *         marker, not decoration.
 */
static enum CommentShape classify_block_shape(const char* source, const struct CommentSpan* span)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Fills `block` from a single span.
 *
 * @param block  Block to fill. Must not be `NULL`.
 * @param source Source bytes. Must not be `NULL`.
 * @param span   Span that is the whole block. Must not be `NULL`.
 */
static void fill_block_from_span(struct CommentBlock* block,
                                 const char* source,
                                 size_t source_len,
                                 const struct CommentSpan* span) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Copies one source line's payload into `line_out` after stripping decoration.
 *
 * @param source      Source bytes. Must not be `NULL`.
 * @param line_start  Inclusive start of the line.
 * @param line_end    Exclusive end of the line, before any newline.
 * @param block       Block being extracted. Must not be `NULL`.
 * @param is_first    Whether this is the opener line.
 * @param is_last     Whether this is the last line of the block.
 * @param arena       Arena that owns the copied text. Must not be `NULL`.
 * @param line_out    Receives the stripped line when it is kept. Must not be `NULL`.
 * @param is_kept_out Receives whether the line belongs in the extracted body. Must not be `NULL`.
 * @param err         Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len     Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_stripped_line(const char* source,
                                size_t line_start,
                                size_t line_end,
                                const struct CommentBlock* block,
                                bool is_first,
                                bool is_last,
                                struct Arena* arena,
                                struct BodyLine* line_out,
                                bool* is_kept_out,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 4, 7, 8, 9)));

/**
 * @brief Classifies a stripped payload as prose, tag, list, code, decoration, or blank.
 *
 * @param text            Terminated payload. Must not be `NULL`.
 * @param text_len        Length of `text`.
 * @param is_in_fence     Whether a fenced code block is open.
 * @param is_in_fence_out Receives the updated fence state. Must not be `NULL`.
 * @return Line kind.
 */
static enum BodyLineKind classify_payload(const char* text,
                                          size_t text_len,
                                          bool is_in_fence,
                                          bool* is_in_fence_out) __attribute__((nonnull(1, 4)));

/**
 * @brief Reports whether `text` is a list item marker line.
 *
 * @param text Terminated payload. Must not be `NULL`.
 * @return `true` when the line starts with `- `, `* `, `+ `, or a numbered item.
 */
static bool is_list_line(const char* text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` is a fence opener or closer.
 *
 * @param text Terminated payload. Must not be `NULL`.
 * @return `true` when the line is at least three backticks or tildes.
 */
static bool is_fence_line(const char* text) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` contains only ASCII decorative punctuation and whitespace.
 *
 * @param text     Payload. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @return `true` when the line is non-empty and has no alphanumeric, UTF-8, or backtick byte.
 */
static bool is_decoration_line(const char* text, size_t text_len) __attribute__((nonnull(1)));

/**
 * @brief Skips hanging-indent whitespace, leaving any extra indent in place.
 *
 * @param source       Source bytes. Must not be `NULL`.
 * @param start        Inclusive start of the line.
 * @param end          Exclusive end of the line.
 * @param hang_columns Display columns of hanging indent to consume.
 * @return Offset of the first kept byte.
 */
static size_t skip_hanging_indent(const char* source, size_t start, size_t end, size_t hang_columns)
    __attribute__((nonnull(1)));

/**
 * @brief Writes `count` spaces to `buffer`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param count  Number of spaces to write.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int emit_indent(struct StringBuffer* buffer, size_t count) __attribute__((nonnull(1)));

/**
 * @brief Writes the line ending selected for `block` to `buffer`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param block  Block whose line-ending style is used. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int emit_newline(struct StringBuffer* buffer, const struct CommentBlock* block)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the original indent whitespace of the line containing `block->start`.
 *
 * @param source         Source bytes. Must not be `NULL`.
 * @param block          Block whose opener indent is recovered. Must not be `NULL`.
 * @param indent_len_out Receives the indent byte length. Must not be `NULL`.
 * @return Pointer to the indent bytes in `source`.
 */
static const char* opener_indent(const char* source,
                                 const struct CommentBlock* block,
                                 size_t* indent_len_out) __attribute__((nonnull(1, 2, 3)));

int comment_group(const char* source,
                  size_t source_len,
                  const struct CommentSpanList* spans,
                  struct Arena* arena,
                  struct CommentBlockList* blocks_out,
                  char* err,
                  size_t err_len) {
  size_t count = 0;
  for (size_t i = 0; i < spans->count; i++) {
    const struct CommentSpan* span = &spans->items[i];
    if (span->is_trailing) {
      continue;
    }
    count++;
    if (span->kind == COMMENT_KIND_LINE) {
      while (is_line_run_continuation(source, spans, i + 1, span->end, span->opener_column)) {
        i++;
        span = &spans->items[i];
      }
    }
  }

  struct CommentBlock* items = NULL;
  if (count > 0) {
    items = arena_calloc(arena, count, sizeof(*items));
    if (items == NULL) {
      return error_report(err, err_len, "out of memory");
    }
    size_t n = 0;
    for (size_t i = 0; i < spans->count; i++) {
      const struct CommentSpan* span = &spans->items[i];
      if (span->is_trailing) {
        continue;
      }
      struct CommentBlock* block = &items[n];
      fill_block_from_span(block, source, source_len, span);
      if (span->kind == COMMENT_KIND_LINE) {
        while (is_line_run_continuation(source, spans, i + 1, spans->items[i].end,
                                        span->opener_column)) {
          i++;
          block->end = spans->items[i].end;
        }
      }
      n++;
    }
  }

  blocks_out->items = items;
  blocks_out->count = count;
  return 0;
}

int comment_extract_body(const char* source,
                         const struct CommentBlock* block,
                         struct Arena* arena,
                         struct BodyLineList* lines_out,
                         char* err,
                         size_t err_len) {
  size_t line_count = 1;
  for (size_t i = block->start; i < block->end; i++) {
    if (source[i] == '\n') {
      line_count++;
    }
  }
  struct BodyLine* items = arena_calloc(arena, line_count, sizeof(*items));
  if (items == NULL) {
    return error_report(err, err_len, "out of memory");
  }

  size_t n = 0;
  bool is_in_fence = false;
  bool is_in_tag = false;
  size_t line_start = block->start;
  for (size_t i = block->start; i <= block->end; i++) {
    if (i < block->end && source[i] != '\n') {
      continue;
    }
    size_t line_end = i;
    if (line_end > line_start && source[line_end - 1] == '\r') {
      line_end--;
    }
    const bool is_first = n == 0 && line_start == block->start;
    const bool is_last = i == block->end;
    bool is_kept = false;
    if (append_stripped_line(source, line_start, line_end, block, is_first, is_last, arena,
                             &items[n], &is_kept, err, err_len) != 0) {
      return -1;
    }
    if (is_kept) {
      items[n].kind = classify_payload(items[n].text, items[n].text_len, is_in_fence, &is_in_fence);
      // Extra indent after decoration is an indented sample, except when it continues a tag.
      if (items[n].kind == BODY_LINE_PROSE && items[n].text_len > 0 &&
          (items[n].text[0] == ' ' || items[n].text[0] == '\t') && !is_in_tag) {
        items[n].kind = BODY_LINE_CODE;
      }
      if (items[n].kind == BODY_LINE_TAG) {
        is_in_tag = true;
      } else if (items[n].kind != BODY_LINE_PROSE) {
        is_in_tag = false;
      }
      n++;
    }
    line_start = i + 1;
    if (i >= block->end) {
      break;
    }
  }

  lines_out->items = items;
  lines_out->count = n;
  return 0;
}

int comment_emit(struct StringBuffer* buffer,
                 const char* source,
                 const struct CommentBlock* block,
                 const struct BodyLineList* lines) {
  size_t indent_len = 0;
  const char* indent = opener_indent(source, block, &indent_len);
  // The original indent already sits in the source gap before `block->start`. Only lines after the
  // first need it written again.

  if (block->shape == COMMENT_SHAPE_LINE) {
    for (size_t i = 0; i < lines->count; i++) {
      if (i > 0) {
        if (emit_newline(buffer, block) != 0 ||
            string_buffer_append_len(buffer, indent, indent_len) != 0) {
          return -1;
        }
      }
      if (string_buffer_append(buffer, "//") != 0) {
        return -1;
      }
      if (lines->items[i].text_len > 0) {
        if (string_buffer_append_char(buffer, ' ') != 0 ||
            string_buffer_append_len(buffer, lines->items[i].text, lines->items[i].text_len) != 0) {
          return -1;
        }
      }
    }
    return 0;
  }

  if (string_buffer_append(buffer, block->is_doxygen_opener ? "/**" : "/*") != 0) {
    return -1;
  }

  size_t prose_index = 0;
  if (!block->is_opener_empty && lines->count > 0) {
    if (string_buffer_append_char(buffer, ' ') != 0 ||
        string_buffer_append_len(buffer, lines->items[0].text, lines->items[0].text_len) != 0) {
      return -1;
    }
    prose_index = 1;
  }

  if (block->shape == COMMENT_SHAPE_STARRED) {
    // When the refilled prose all fits on the opener line there is no continuation to decorate, so
    // the closer trails that line rather than sitting alone below it. A lone closer is a shape this
    // function never reproduces from its own output, which would make `--check` report a rewrap for
    // a file `--in-place` just wrote.
    if (prose_index > 0 && prose_index >= lines->count) {
      return string_buffer_append(buffer, " */");
    }

    for (size_t i = prose_index; i < lines->count; i++) {
      if (emit_newline(buffer, block) != 0 ||
          string_buffer_append_len(buffer, indent, indent_len) != 0 ||
          string_buffer_append(buffer, " *") != 0) {
        return -1;
      }
      if (lines->items[i].text_len > 0) {
        if (string_buffer_append_char(buffer, ' ') != 0 ||
            string_buffer_append_len(buffer, lines->items[i].text, lines->items[i].text_len) != 0) {
          return -1;
        }
      }
    }
    if (emit_newline(buffer, block) != 0 ||
        string_buffer_append_len(buffer, indent, indent_len) != 0 ||
        string_buffer_append(buffer, " */") != 0) {
      return -1;
    }
    return 0;
  }

  for (size_t i = prose_index; i < lines->count; i++) {
    if (emit_newline(buffer, block) != 0) {
      return -1;
    }
    if (lines->items[i].text_len == 0) {
      continue;
    }
    if (string_buffer_append_len(buffer, indent, indent_len) != 0 || emit_indent(buffer, 3) != 0 ||
        string_buffer_append_len(buffer, lines->items[i].text, lines->items[i].text_len) != 0) {
      return -1;
    }
  }
  if (string_buffer_append(buffer, " */") != 0) {
    return -1;
  }
  return 0;
}

size_t comment_prefix_columns(const struct CommentBlock* block) {
  return block->indent_columns + 3;
}

size_t comment_first_prefix_columns(const struct CommentBlock* block) {
  const size_t marker_columns = block->is_doxygen_opener && !block->is_opener_empty ? 4 : 3;
  return block->indent_columns + marker_columns;
}

static bool is_line_run_continuation(const char* source,
                                     const struct CommentSpanList* spans,
                                     size_t index,
                                     size_t previous_end,
                                     size_t indent_columns) {
  if (index >= spans->count) {
    return false;
  }
  const struct CommentSpan* next = &spans->items[index];
  if (next->kind != COMMENT_KIND_LINE || next->is_trailing ||
      next->opener_column != indent_columns) {
    return false;
  }
  if (next->start == previous_end + 1 && source[previous_end] == '\n') {
    return true;
  }
  if (next->start == previous_end + 2 && source[previous_end] == '\r' &&
      source[previous_end + 1] == '\n') {
    return true;
  }
  return false;
}

static enum CommentShape classify_block_shape(const char* source, const struct CommentSpan* span) {
  bool has_star = false;
  const size_t star_column = span->opener_column + 1;
  size_t i = span->start;
  while (i < span->end && source[i] != '\n') {
    i++;
  }
  if (i < span->end && source[i] == '\n') {
    i++;
  }
  while (i < span->end) {
    size_t column = 0;
    while (i < span->end && (source[i] == ' ' || source[i] == '\t')) {
      if (source[i] == '\t') {
        column += (size_t)TAB_WIDTH - (column % (size_t)TAB_WIDTH);
      } else {
        column++;
      }
      i++;
    }
    if (i < span->end && source[i] == '*' && column == star_column) {
      if (i + 1 >= span->end || source[i + 1] != '/') {
        has_star = true;
      }
    }
    while (i < span->end && source[i] != '\n') {
      i++;
    }
    if (i < span->end && source[i] == '\n') {
      i++;
    }
  }
  if (has_star) {
    return COMMENT_SHAPE_STARRED;
  }
  return COMMENT_SHAPE_HANGING;
}

static void fill_block_from_span(struct CommentBlock* block,
                                 const char* source,
                                 size_t source_len,
                                 const struct CommentSpan* span) {
  block->start = span->start;
  block->end = span->end;
  block->indent_columns = span->opener_column;
  block->has_crlf_newlines = false;
  for (size_t i = span->start; i < span->end; i++) {
    if (source[i] == '\n') {
      block->has_crlf_newlines = i > span->start && source[i - 1] == '\r';
      break;
    }
  }
  if (span->end < source_len && source[span->end] == '\r' && span->end + 1 < source_len &&
      source[span->end + 1] == '\n') {
    block->has_crlf_newlines = true;
  }
  block->is_doxygen_opener = false;
  block->is_opener_empty = false;
  if (span->kind == COMMENT_KIND_LINE) {
    block->shape = COMMENT_SHAPE_LINE;
    return;
  }
  block->shape = classify_block_shape(source, span);
  if (span->end >= span->start + 3 && source[span->start] == '/' &&
      source[span->start + 1] == '*' && source[span->start + 2] == '*') {
    block->is_doxygen_opener = true;
  }
  size_t i = span->start + 2;
  if (block->is_doxygen_opener) {
    i++;
  }
  while (i < span->end && (source[i] == ' ' || source[i] == '\t')) {
    i++;
  }
  if (i >= span->end || source[i] == '\n' ||
      (source[i] == '\r' && i + 1 < span->end && source[i + 1] == '\n') ||
      (source[i] == '*' && i + 1 < span->end && source[i + 1] == '/')) {
    block->is_opener_empty = true;
  }
}

static int append_stripped_line(const char* source,
                                size_t line_start,
                                size_t line_end,
                                const struct CommentBlock* block,
                                bool is_first,
                                bool is_last,
                                struct Arena* arena,
                                struct BodyLine* line_out,
                                bool* is_kept_out,
                                char* err,
                                size_t err_len) {
  size_t start = line_start;
  size_t end = line_end;
  *is_kept_out = true;

  if (block->shape == COMMENT_SHAPE_LINE) {
    while (start < end && (source[start] == ' ' || source[start] == '\t')) {
      start++;
    }
    if (start + 1 < end && source[start] == '/' && source[start + 1] == '/') {
      start += 2;
      if (start < end && source[start] == ' ') {
        start++;
      }
    }
  } else {
    if (is_last) {
      while (end > start && (source[end - 1] == ' ' || source[end - 1] == '\t')) {
        end--;
      }
      if (end >= start + 2 && source[end - 2] == '*' && source[end - 1] == '/') {
        end -= 2;
        while (end > start && (source[end - 1] == ' ' || source[end - 1] == '\t')) {
          end--;
        }
      }
    }
    if (is_first) {
      while (start < end && (source[start] == ' ' || source[start] == '\t')) {
        start++;
      }
      if (start + 1 < end && source[start] == '/' && source[start + 1] == '*') {
        start += 2;
        if (start < end && source[start] == '*') {
          start++;
        }
        if (start < end && source[start] == ' ') {
          start++;
        }
      }
    } else if (block->shape == COMMENT_SHAPE_STARRED) {
      while (start < end && (source[start] == ' ' || source[start] == '\t')) {
        start++;
      }
      if (start < end && source[start] == '*') {
        start++;
        if (start < end && source[start] == ' ') {
          start++;
        }
      }
    } else {
      const size_t hang_columns = block->indent_columns + (block->is_doxygen_opener ? 4 : 3);
      start = skip_hanging_indent(source, start, end, hang_columns);
    }
    if ((is_last && start == end) || (is_first && block->is_opener_empty && start == end)) {
      *is_kept_out = false;
      return 0;
    }
  }

  const size_t len = end > start ? end - start : 0;
  char* copy = arena_strndup(arena, len > 0 ? source + start : "", len);
  if (copy == NULL) {
    return error_report(err, err_len, "out of memory");
  }
  line_out->text = copy;
  line_out->text_len = len;
  return 0;
}

static enum BodyLineKind classify_payload(const char* text,
                                          size_t text_len,
                                          bool is_in_fence,
                                          bool* is_in_fence_out) {
  *is_in_fence_out = is_in_fence;
  if (text_len == 0) {
    return BODY_LINE_BLANK;
  }
  if (is_fence_line(text)) {
    *is_in_fence_out = !is_in_fence;
    return BODY_LINE_CODE;
  }
  if (is_in_fence) {
    return BODY_LINE_CODE;
  }
  if (text_len >= 4 && text[0] == ' ' && text[1] == ' ' && text[2] == ' ' && text[3] == ' ') {
    return BODY_LINE_CODE;
  }
  struct DoxygenTag tag;
  if (doxygen_has_tag(text, &tag)) {
    return BODY_LINE_TAG;
  }
  if (is_list_line(text)) {
    return BODY_LINE_LIST;
  }
  if (is_decoration_line(text, text_len)) {
    return BODY_LINE_DECORATION;
  }
  return BODY_LINE_PROSE;
}

static bool is_list_line(const char* text) {
  if ((text[0] == '-' || text[0] == '*' || text[0] == '+') && text[1] == ' ') {
    return true;
  }
  size_t i = 0;
  if (!ascii_is_digit((unsigned char)text[0])) {
    return false;
  }
  while (ascii_is_digit((unsigned char)text[i])) {
    i++;
  }
  return text[i] == '.' && text[i + 1] == ' ';
}

static bool is_fence_line(const char* text) {
  if (text[0] != '`' && text[0] != '~') {
    return false;
  }
  const char marker = text[0];
  size_t n = 0;
  while (text[n] == marker) {
    n++;
  }
  return n >= 3;
}

static bool is_decoration_line(const char* text, size_t text_len) {
  bool has_punctuation = false;
  for (size_t i = 0; i < text_len; i++) {
    const unsigned char c = (unsigned char)text[i];
    if (c >= 0x80 || ascii_is_alphanumeric(c) || c == '`') {
      return false;
    }
    if (c != ' ' && c != '\t') {
      has_punctuation = true;
    }
  }
  return has_punctuation;
}

static size_t skip_hanging_indent(const char* source,
                                  size_t start,
                                  size_t end,
                                  size_t hang_columns) {
  size_t i = start;
  size_t columns = 0;
  while (i < end && (source[i] == ' ' || source[i] == '\t') && columns < hang_columns) {
    if (source[i] == '\t') {
      columns += (size_t)TAB_WIDTH - (columns % (size_t)TAB_WIDTH);
    } else {
      columns++;
    }
    i++;
  }
  return i;
}

static int emit_indent(struct StringBuffer* buffer, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (string_buffer_append_char(buffer, ' ') != 0) {
      return -1;
    }
  }
  return 0;
}

static int emit_newline(struct StringBuffer* buffer, const struct CommentBlock* block) {
  return string_buffer_append(buffer, block->has_crlf_newlines ? "\r\n" : "\n");
}

static const char* opener_indent(const char* source,
                                 const struct CommentBlock* block,
                                 size_t* indent_len_out) {
  size_t line = block->start;
  while (line > 0 && source[line - 1] != '\n') {
    line--;
  }
  *indent_len_out = block->start - line;
  return source + line;
}
