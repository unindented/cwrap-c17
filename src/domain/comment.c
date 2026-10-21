#include "domain/comment.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/ascii.h"
#include "domain/doxygen.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"

/** Fenced region open while body lines are classified. */
enum PayloadFence {
  /** No fence is open. */
  PAYLOAD_FENCE_NONE,

  /** A Markdown fence of three or more backticks or tildes, closed by another. */
  PAYLOAD_FENCE_MARKDOWN,

  /** A Doxygen `@code` region, closed by `@endcode`. */
  PAYLOAD_FENCE_CODE,

  /** A Doxygen `@verbatim` region, closed by `@endverbatim`. */
  PAYLOAD_FENCE_VERBATIM,
};

/**
 * Leading text of the tool directives that a body line keeps on a line of its own. A payload
 * matches when it starts with an entry and, if the entry ends in a letter, digit, or underscore,
 * the next byte is none of those. To recognize another directive, add its leading text here.
 */
static const struct Str tool_directives[] = {
    STR_INIT("cppcheck-suppress"), STR_INIT("NOLINT"),       STR_INIT("NOLINTNEXTLINE"),
    STR_INIT("NOLINTBEGIN"),       STR_INIT("NOLINTEND"),    STR_INIT("clang-format off"),
    STR_INIT("clang-format on"),   STR_INIT("IWYU pragma:"),
};

/** Classification state carried from one body line to the next. */
struct PayloadState {
  /** Fence open after the previous line. */
  enum PayloadFence fence;

  /** Whether the previous line is a tag or one of its continuations. */
  bool is_in_tag;
};

/**
 * @brief Reports whether `span` is rewritten rather than copied unchanged.
 *
 * A trailing comment and a `//` comment continued by a backslash line splice are copied unchanged.
 * The spliced physical lines carry no `//` marker, so no refill can re-emit them as a line comment.
 *
 * @param source Source bytes.
 * @param span   Comment span. Must not be `NULL`.
 * @return `true` when the span is rewritten, or `false` when it is copied unchanged.
 */
static bool is_span_rewritable(struct Str source, const struct CommentSpan* span)
    __attribute__((nonnull(2)));

/**
 * @brief Reports whether `spans[index]` can extend the line-comment run that `first` opens.
 *
 * The candidate must sit on the line after the previous span, preceded by nothing but horizontal
 * whitespace, at the same indent column and with the same marker as `first`.
 *
 * @param source  Source bytes.
 * @param spans   Span list. Must not be `NULL`.
 * @param index   Candidate span index.
 * @param run_end Exclusive end of the run so far, which is the end of its last span.
 * @param first   First span of the run, whose column and marker the candidate must match. Must not
 *                be `NULL`.
 * @return `true` when the span continues the run.
 */
static bool is_line_run_continuation(struct Str source,
                                     const struct CommentSpanList* spans,
                                     ptrdiff_t index,
                                     ptrdiff_t run_end,
                                     const struct CommentSpan* first)
    __attribute__((nonnull(2, 5)));

/**
 * @brief Returns the opener marker of `span`.
 *
 * @param source Source bytes.
 * @param span   Comment span. Must not be `NULL`.
 * @return `//`, `///`, or `//!` for a line comment, or slash-star, slash-star-star, or
 *         slash-star-bang for a block comment, in static storage.
 */
static struct Str span_opener(struct Str source, const struct CommentSpan* span)
    __attribute__((nonnull(2)));

/**
 * @brief Reports whether `block` holds any byte of prose between its markers.
 *
 * @param source Source bytes.
 * @param block  Grouped block. Must not be `NULL`.
 * @return `true` when an ASCII letter or digit, or a non-ASCII byte, appears in the block, or
 *         `false` for a banner or other decoration-only block that is copied unchanged.
 */
static bool has_alphanumeric_payload(struct Str source, const struct CommentBlock* block)
    __attribute__((nonnull(2)));

/**
 * @brief Classifies a slash-star span as starred or hanging from its continuation lines.
 *
 * @param source Source bytes.
 * @param span   Block-comment span. Must not be `NULL`.
 * @return `COMMENT_SHAPE_STARRED` when a continuation `*` sits at the decoration column
 *         (`opener_column + 1`), otherwise hanging. A `*` at the hanging-text column is a list
 *         marker, not decoration.
 */
static enum CommentShape classify_block_shape(struct Str source, const struct CommentSpan* span)
    __attribute__((nonnull(2)));

/**
 * @brief Fills `block` from a single span.
 *
 * @param block  Block to fill. Must not be `NULL`.
 * @param source Source bytes.
 * @param span   Span that is the whole block. Must not be `NULL`.
 */
static void fill_block_from_span(struct CommentBlock* block,
                                 struct Str source,
                                 const struct CommentSpan* span) __attribute__((nonnull(1, 3)));

/**
 * @brief Points `line_out` at one source line's payload after stripping decoration.
 *
 * @param source     Source bytes. The payload is a view into it.
 * @param block      Block being extracted. Must not be `NULL`.
 * @param line_start Inclusive start of the line.
 * @param line_end   Exclusive end of the line, before any newline.
 * @param is_first   Whether this is the opener line.
 * @param is_last    Whether this is the last line of the block.
 * @param line_out   Receives the stripped line when it is kept. Must not be `NULL`.
 * @return `true` when the line belongs in the extracted body, or `false` for a closer or empty
 *         opener line that is dropped.
 */
static bool comment_extract_body_line(struct Str source,
                                      const struct CommentBlock* block,
                                      ptrdiff_t line_start,
                                      ptrdiff_t line_end,
                                      bool is_first,
                                      bool is_last,
                                      struct BodyLine* line_out) __attribute__((nonnull(2, 7)));

/**
 * @brief Returns the length of the blanks, the comment opener, and one space that open `text`.
 *
 * @param text   Line bytes.
 * @param opener Opener of the block the line belongs to.
 * @return Bytes to skip: the leading blanks, plus `opener` and one following space when `text`
 *         continues with `opener`.
 */
static ptrdiff_t opener_prefix_len(struct Str text, struct Str opener);

/**
 * @brief Classifies a stripped payload as prose, tag, list, code, decoration, directive, or blank.
 *
 * @param state      Fence and tag state carried from the previous line, updated for the next one.
 *                   Must not be `NULL`.
 * @param text       Payload.
 * @param is_doxygen Whether the block recognizes Doxygen commands.
 * @return Line kind.
 */
static enum BodyLineKind comment_extract_body_classify(struct PayloadState* state,
                                                       struct Str text,
                                                       bool is_doxygen) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` starts with the Doxygen command `command`.
 *
 * @param text    Payload bytes.
 * @param command Command word without its marker.
 * @return `true` when the parsed command word equals `command`.
 */
static bool has_doxygen_command(struct Str text, struct Str command);

/**
 * @brief Reports whether `c` can continue a word, so a directive that ends before it is not whole.
 *
 * @param c Byte to test.
 * @return `true` for an ASCII letter, digit, or underscore.
 */
static bool is_word_byte(unsigned char c);

/**
 * @brief Returns the number of leading spaces and tabs in `text`.
 *
 * @param text Payload bytes.
 * @return Byte length of the leading whitespace.
 */
static ptrdiff_t leading_space_len(struct Str text);

/**
 * @brief Reports whether `text` is a fence opener or closer.
 *
 * @param text Payload.
 * @return `true` when the line is at least three backticks or tildes.
 */
static bool is_fence_line(struct Str text);

/**
 * @brief Reports whether `text` contains only ASCII decorative punctuation and whitespace.
 *
 * @param text Payload.
 * @return `true` when the line is non-empty and has no alphanumeric, UTF-8, or backtick byte.
 */
static bool is_decoration_line(struct Str text);

/**
 * @brief Skips hanging-indent whitespace, leaving any extra indent in place.
 *
 * @param source       Source bytes.
 * @param start        Inclusive start of the line.
 * @param end          Exclusive end of the line.
 * @param hang_columns Display columns of hanging indent to consume.
 * @return Offset of the first kept byte.
 */
static ptrdiff_t skip_hanging_indent(struct Str source,
                                     ptrdiff_t start,
                                     ptrdiff_t end,
                                     ptrdiff_t hang_columns);

/**
 * @brief Writes `count` spaces to `buffer`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param count  Number of spaces to write.
 */
static void emit_indent(struct Buf* buffer, ptrdiff_t count) __attribute__((nonnull(1)));

/**
 * @brief Writes the line ending selected for `block` to `buffer`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param block  Block whose line-ending style is used. Must not be `NULL`.
 */
static void emit_newline(struct Buf* buffer, const struct CommentBlock* block)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Returns the original indent whitespace of the line containing `block->start`.
 *
 * @param source Source bytes.
 * @param block  Block whose opener indent is recovered. Must not be `NULL`.
 * @return View of the indent bytes in `source`.
 */
static struct Str opener_indent(struct Str source, const struct CommentBlock* block)
    __attribute__((nonnull(2)));

struct CommentBlockList comment_group(struct Str source,
                                      const struct CommentSpanList* spans,
                                      struct Arena* arena) {
  // Each block holds at least one span, so the span count bounds the block count.
  struct CommentBlock* items = arena_new(arena, spans->count, struct CommentBlock);

  ptrdiff_t n = 0;
  for (ptrdiff_t i = 0; i < spans->count; i++) {
    const struct CommentSpan* span = &spans->items[i];
    if (!is_span_rewritable(source, span)) {
      continue;
    }
    struct CommentBlock* block = &items[n];
    fill_block_from_span(block, source, span);
    if (span->kind == COMMENT_KIND_LINE) {
      while (is_line_run_continuation(source, spans, i + 1, spans->items[i].end, span)) {
        i++;
        block->end = spans->items[i].end;
      }
    }
    // A block without prose, such as a banner, keeps its exact bytes. The next block reuses the
    // slot.
    if (has_alphanumeric_payload(source, block)) {
      n++;
    }
  }

  return (struct CommentBlockList){.items = items, .count = n};
}

struct BodyLineList comment_extract_body(struct Str source,
                                         const struct CommentBlock* block,
                                         struct Arena* arena) {
  ptrdiff_t line_count = 1;
  for (ptrdiff_t i = block->start; i < block->end; i++) {
    if (source.data[i] == '\n') {
      line_count++;
    }
  }
  struct BodyLine* items = arena_new(arena, line_count, struct BodyLine);

  ptrdiff_t n = 0;
  struct PayloadState state = {.fence = PAYLOAD_FENCE_NONE, .is_in_tag = false};
  ptrdiff_t line_start = block->start;
  for (ptrdiff_t i = block->start; i <= block->end; i++) {
    if (i < block->end && source.data[i] != '\n') {
      continue;
    }
    ptrdiff_t line_end = i;
    if (line_end > line_start && source.data[line_end - 1] == '\r') {
      line_end--;
    }
    const bool is_first = n == 0 && line_start == block->start;
    const bool is_last = i == block->end;
    if (comment_extract_body_line(source, block, line_start, line_end, is_first, is_last,
                                  &items[n])) {
      struct BodyLine* line = &items[n];
      line->kind = comment_extract_body_classify(&state, line->text, block->is_doxygen);
      // Only a tag continuation is prose with an indent, which is not part of its description.
      if (line->kind == BODY_LINE_PROSE) {
        line->text = str_tail(line->text, leading_space_len(line->text));
      }
      n++;
    }
    line_start = i + 1;
    if (i >= block->end) {
      break;
    }
  }

  return (struct BodyLineList){.items = items, .count = n};
}

void comment_emit(struct Buf* buffer,
                  struct Str source,
                  const struct CommentBlock* block,
                  const struct BodyLineList* lines) {
  const struct Str indent = opener_indent(source, block);
  // The original indent already sits in the source gap before `block->start`. Only lines after the
  // first need it written again.

  if (block->shape == COMMENT_SHAPE_LINE) {
    for (ptrdiff_t i = 0; i < lines->count; i++) {
      if (i > 0) {
        emit_newline(buffer, block);
        buf_append(buffer, indent);
      }
      buf_append(buffer, block->opener);
      if (lines->items[i].text.len > 0) {
        buf_append_char(buffer, ' ');
        buf_append(buffer, lines->items[i].text);
      }
    }
    return;
  }

  buf_append(buffer, block->opener);

  ptrdiff_t prose_index = 0;
  if (!block->is_opener_empty && lines->count > 0) {
    buf_append_char(buffer, ' ');
    buf_append(buffer, lines->items[0].text);
    prose_index = 1;
  }

  if (block->shape == COMMENT_SHAPE_STARRED) {
    // A lone closer below a one-line body is a shape this function never reproduces from its own
    // output, which would make `--check` report a rewrap for a file `--in-place` just wrote.
    if (comment_has_opener_closer(block, lines)) {
      buf_append(buffer, S(" */"));
      return;
    }

    for (ptrdiff_t i = prose_index; i < lines->count; i++) {
      emit_newline(buffer, block);
      buf_append(buffer, indent);
      buf_append(buffer, S(" *"));
      if (lines->items[i].text.len > 0) {
        buf_append_char(buffer, ' ');
        buf_append(buffer, lines->items[i].text);
      }
    }
    emit_newline(buffer, block);
    buf_append(buffer, indent);
    buf_append(buffer, S(" */"));
    return;
  }

  for (ptrdiff_t i = prose_index; i < lines->count; i++) {
    emit_newline(buffer, block);
    if (lines->items[i].text.len == 0) {
      continue;
    }
    // A continuation starts in the column after the opener and its space, which extraction strips.
    buf_append(buffer, indent);
    emit_indent(buffer, block->opener.len + 1);
    buf_append(buffer, lines->items[i].text);
  }
  buf_append(buffer, S(" */"));
}

bool comment_has_opener_closer(const struct CommentBlock* block, const struct BodyLineList* lines) {
  return block->shape == COMMENT_SHAPE_STARRED && !block->is_opener_empty && lines->count == 1;
}

ptrdiff_t comment_prefix_columns(const struct CommentBlock* block) {
  const ptrdiff_t marker_columns =
      block->shape == COMMENT_SHAPE_STARRED ? S(" * ").len : block->opener.len + 1;
  return block->indent_columns + marker_columns;
}

ptrdiff_t comment_prefix_columns_first(const struct CommentBlock* block) {
  if (block->is_opener_empty) {
    return comment_prefix_columns(block);
  }
  return block->indent_columns + block->opener.len + 1;
}

bool comment_is_list_line(struct Str text) {
  if (text.len >= 2 && (text.data[0] == '-' || text.data[0] == '*' || text.data[0] == '+') &&
      text.data[1] == ' ') {
    return true;
  }
  ptrdiff_t i = 0;
  while (i < text.len && ascii_is_digit((unsigned char)text.data[i])) {
    i++;
  }
  return i > 0 && i + 1 < text.len && text.data[i] == '.' && text.data[i + 1] == ' ';
}

bool comment_is_tool_directive(struct Str text) {
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(tool_directives) / sizeof(tool_directives[0]));
       i++) {
    const struct Str directive = tool_directives[i];
    if (!str_startswith(text, directive)) {
      continue;
    }
    // A directive that ends the payload ends a word too.
    const unsigned char last = (unsigned char)directive.data[directive.len - 1];
    if (directive.len == text.len || !is_word_byte(last) ||
        !is_word_byte((unsigned char)text.data[directive.len])) {
      return true;
    }
  }
  return false;
}

static bool is_span_rewritable(struct Str source, const struct CommentSpan* span) {
  if (span->is_trailing) {
    return false;
  }
  const struct Str text = {source.data + span->start, span->end - span->start};
  return span->kind != COMMENT_KIND_LINE || !str_cut(text, '\n').is_found;
}

static bool is_line_run_continuation(struct Str source,
                                     const struct CommentSpanList* spans,
                                     ptrdiff_t index,
                                     ptrdiff_t run_end,
                                     const struct CommentSpan* first) {
  if (index >= spans->count) {
    return false;
  }
  const struct CommentSpan* next = &spans->items[index];
  if (next->kind != COMMENT_KIND_LINE || !is_span_rewritable(source, next) ||
      next->opener_column != first->opener_column ||
      !str_eq(span_opener(source, next), span_opener(source, first))) {
    return false;
  }
  // Only the line break and the next line's indent may separate the two spans. The column check
  // above keeps that indent at the run's own.
  ptrdiff_t i = run_end;
  if (i < next->start && source.data[i] == '\r') {
    i++;
  }
  if (i >= next->start || source.data[i] != '\n') {
    return false;
  }
  i++;
  while (i < next->start && (source.data[i] == ' ' || source.data[i] == '\t')) {
    i++;
  }
  return i == next->start;
}

static struct Str span_opener(struct Str source, const struct CommentSpan* span) {
  const struct Str text = {source.data + span->start, span->end - span->start};
  if (span->kind == COMMENT_KIND_LINE) {
    if (text.len >= 3 && text.data[2] == '!') {
      return S("//!");
    }
    // Four or more slashes are a plain comment whose payload starts with slashes.
    if (text.len >= 3 && text.data[2] == '/' && (text.len == 3 || text.data[3] != '/')) {
      return S("///");
    }
    return S("//");
  }
  if (text.len >= 3 && text.data[2] == '*') {
    return S("/**");
  }
  if (text.len >= 3 && text.data[2] == '!') {
    return S("/*!");
  }
  return S("/*");
}

static bool has_alphanumeric_payload(struct Str source, const struct CommentBlock* block) {
  for (ptrdiff_t i = block->start; i < block->end; i++) {
    const unsigned char c = (unsigned char)source.data[i];
    if (c >= 0x80 || ascii_is_alphanumeric(c)) {
      return true;
    }
  }
  return false;
}

static enum CommentShape classify_block_shape(struct Str source, const struct CommentSpan* span) {
  bool has_star = false;
  const ptrdiff_t star_column = span->opener_column + 1;
  ptrdiff_t i = span->start;
  while (i < span->end && source.data[i] != '\n') {
    i++;
  }
  if (i < span->end && source.data[i] == '\n') {
    i++;
  }
  while (i < span->end) {
    ptrdiff_t column = 0;
    while (i < span->end && (source.data[i] == ' ' || source.data[i] == '\t')) {
      column = lex_advance_column(column, (unsigned char)source.data[i]);
      i++;
    }
    if (i < span->end && source.data[i] == '*' && column == star_column) {
      if (i + 1 >= span->end || source.data[i + 1] != '/') {
        has_star = true;
      }
    }
    while (i < span->end && source.data[i] != '\n') {
      i++;
    }
    if (i < span->end && source.data[i] == '\n') {
      i++;
    }
  }
  if (has_star) {
    return COMMENT_SHAPE_STARRED;
  }
  return COMMENT_SHAPE_HANGING;
}

static void fill_block_from_span(struct CommentBlock* block,
                                 struct Str source,
                                 const struct CommentSpan* span) {
  block->start = span->start;
  block->end = span->end;
  block->indent_columns = span->opener_column;
  block->has_crlf_newlines = false;
  for (ptrdiff_t i = span->start; i < span->end; i++) {
    if (source.data[i] == '\n') {
      block->has_crlf_newlines = i > span->start && source.data[i - 1] == '\r';
      break;
    }
  }
  if (span->end < source.len && source.data[span->end] == '\r' && span->end + 1 < source.len &&
      source.data[span->end + 1] == '\n') {
    block->has_crlf_newlines = true;
  }
  block->opener = span_opener(source, span);
  block->is_doxygen = !str_eq(block->opener, S("//")) && !str_eq(block->opener, S("/*"));
  block->is_opener_empty = false;
  if (span->kind == COMMENT_KIND_LINE) {
    block->shape = COMMENT_SHAPE_LINE;
    return;
  }
  block->shape = classify_block_shape(source, span);
  ptrdiff_t i = span->start + block->opener.len;
  while (i < span->end && (source.data[i] == ' ' || source.data[i] == '\t')) {
    i++;
  }
  if (i >= span->end || source.data[i] == '\n' ||
      (source.data[i] == '\r' && i + 1 < span->end && source.data[i + 1] == '\n')) {
    block->is_opener_empty = true;
  }
}

static bool comment_extract_body_line(struct Str source,
                                      const struct CommentBlock* block,
                                      ptrdiff_t line_start,
                                      ptrdiff_t line_end,
                                      bool is_first,
                                      bool is_last,
                                      struct BodyLine* line_out) {
  ptrdiff_t start = line_start;
  ptrdiff_t end = line_end;

  if (block->shape == COMMENT_SHAPE_LINE) {
    start += opener_prefix_len((struct Str){source.data + start, end - start}, block->opener);
  } else {
    if (is_last) {
      while (end > start && (source.data[end - 1] == ' ' || source.data[end - 1] == '\t')) {
        end--;
      }
      if (end >= start + 2 && source.data[end - 2] == '*' && source.data[end - 1] == '/') {
        end -= 2;
        while (end > start && (source.data[end - 1] == ' ' || source.data[end - 1] == '\t')) {
          end--;
        }
      }
    }
    if (is_first) {
      start += opener_prefix_len((struct Str){source.data + start, end - start}, block->opener);
    } else if (block->shape == COMMENT_SHAPE_STARRED) {
      start += leading_space_len((struct Str){source.data + start, end - start});
      if (start < end && source.data[start] == '*') {
        start++;
        if (start < end && source.data[start] == ' ') {
          start++;
        }
      }
    } else {
      start = skip_hanging_indent(source, start, end, comment_prefix_columns(block));
    }
    if ((is_last && start == end) || (is_first && block->is_opener_empty && start == end)) {
      return false;
    }
  }

  // The payload is a view into the source, because nothing writes to it: a refill builds new text
  // rather than editing it.
  line_out->text = (struct Str){source.data + start, end - start};
  return true;
}

static ptrdiff_t opener_prefix_len(struct Str text, struct Str opener) {
  ptrdiff_t len = leading_space_len(text);
  if (str_startswith(str_tail(text, len), opener)) {
    len += opener.len;
    if (len < text.len && text.data[len] == ' ') {
      len++;
    }
  }
  return len;
}

static enum BodyLineKind comment_extract_body_classify(struct PayloadState* state,
                                                       struct Str text,
                                                       bool is_doxygen) {
  const bool is_in_tag = state->is_in_tag;
  state->is_in_tag = false;
  if (text.len == 0) {
    return BODY_LINE_BLANK;
  }
  const ptrdiff_t indent_len = leading_space_len(text);
  const struct Str unindented = str_tail(text, indent_len);
  if (state->fence == PAYLOAD_FENCE_MARKDOWN) {
    if (is_fence_line(text)) {
      state->fence = PAYLOAD_FENCE_NONE;
    }
    return BODY_LINE_CODE;
  }
  if (state->fence != PAYLOAD_FENCE_NONE) {
    const struct Str closer = state->fence == PAYLOAD_FENCE_CODE ? S("endcode") : S("endverbatim");
    if (has_doxygen_command(unindented, closer)) {
      state->fence = PAYLOAD_FENCE_NONE;
    }
    return BODY_LINE_CODE;
  }
  if (is_fence_line(text)) {
    state->fence = PAYLOAD_FENCE_MARKDOWN;
    return BODY_LINE_CODE;
  }
  if (is_doxygen && has_doxygen_command(unindented, S("code"))) {
    state->fence = PAYLOAD_FENCE_CODE;
    return BODY_LINE_CODE;
  }
  if (is_doxygen && has_doxygen_command(unindented, S("verbatim"))) {
    state->fence = PAYLOAD_FENCE_VERBATIM;
    return BODY_LINE_CODE;
  }
  if (comment_is_tool_directive(text)) {
    return BODY_LINE_DIRECTIVE;
  }
  // Extra indent after decoration marks a sample, unless the line continues a tag. A continuation
  // may sit at any depth, so a description refilled at another width still reads as one.
  if (indent_len > 0) {
    if (is_in_tag && !comment_is_list_line(unindented) && !is_decoration_line(unindented)) {
      state->is_in_tag = true;
      return BODY_LINE_PROSE;
    }
    return BODY_LINE_CODE;
  }
  struct DoxygenTag tag;
  if (is_doxygen && doxygen_has_tag(text, &tag)) {
    state->is_in_tag = true;
    return BODY_LINE_TAG;
  }
  if (comment_is_list_line(text)) {
    return BODY_LINE_LIST;
  }
  if (is_decoration_line(text)) {
    return BODY_LINE_DECORATION;
  }
  state->is_in_tag = is_in_tag;
  return BODY_LINE_PROSE;
}

static bool has_doxygen_command(struct Str text, struct Str command) {
  struct DoxygenTag tag;
  return doxygen_has_tag(text, &tag) && str_eq(tag.command, command);
}

static bool is_word_byte(unsigned char c) {
  return ascii_is_alphanumeric(c) || c == '_';
}

static ptrdiff_t leading_space_len(struct Str text) {
  ptrdiff_t len = 0;
  while (len < text.len && (text.data[len] == ' ' || text.data[len] == '\t')) {
    len++;
  }
  return len;
}

static bool is_fence_line(struct Str text) {
  if (text.len == 0 || (text.data[0] != '`' && text.data[0] != '~')) {
    return false;
  }
  const char marker = text.data[0];
  ptrdiff_t n = 0;
  while (n < text.len && text.data[n] == marker) {
    n++;
  }
  return n >= 3;
}

static bool is_decoration_line(struct Str text) {
  bool has_punctuation = false;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (c >= 0x80 || ascii_is_alphanumeric(c) || c == '`') {
      return false;
    }
    if (c != ' ' && c != '\t') {
      has_punctuation = true;
    }
  }
  return has_punctuation;
}

static ptrdiff_t skip_hanging_indent(struct Str source,
                                     ptrdiff_t start,
                                     ptrdiff_t end,
                                     ptrdiff_t hang_columns) {
  ptrdiff_t i = start;
  ptrdiff_t columns = 0;
  while (i < end && (source.data[i] == ' ' || source.data[i] == '\t') && columns < hang_columns) {
    columns = lex_advance_column(columns, (unsigned char)source.data[i]);
    i++;
  }
  return i;
}

static void emit_indent(struct Buf* buffer, ptrdiff_t count) {
  for (ptrdiff_t i = 0; i < count; i++) {
    buf_append_char(buffer, ' ');
  }
}

static void emit_newline(struct Buf* buffer, const struct CommentBlock* block) {
  buf_append(buffer, block->has_crlf_newlines ? S("\r\n") : S("\n"));
}

static struct Str opener_indent(struct Str source, const struct CommentBlock* block) {
  ptrdiff_t line = block->start;
  while (line > 0 && source.data[line - 1] != '\n') {
    line--;
  }
  return (struct Str){source.data + line, block->start - line};
}
