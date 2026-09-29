#include "domain/comment.h"

#include <stdbool.h>
#include <string.h>

#include "core/ascii.h"
#include "core/error.h"
#include "domain/doxygen.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

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
static const char* const tool_directives[] = {
    "cppcheck-suppress", "NOLINT",           "NOLINTNEXTLINE",  "NOLINTBEGIN",
    "NOLINTEND",         "clang-format off", "clang-format on", "IWYU pragma:",
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
 * @param source Source bytes. Must not be `NULL`.
 * @param span   Comment span. Must not be `NULL`.
 * @return `true` when the span is rewritten, or `false` when it is copied unchanged.
 */
static bool is_span_rewritable(const char* source, const struct CommentSpan* span)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether `spans[index]` can extend the line-comment run that `first` opens.
 *
 * The candidate must sit on the line after the previous span, preceded by nothing but horizontal
 * whitespace, at the same indent column and with the same marker as `first`.
 *
 * @param source       Source bytes. Must not be `NULL`.
 * @param spans        Span list. Must not be `NULL`.
 * @param index        Candidate span index.
 * @param previous_end Exclusive end of the previous span in the run.
 * @param first        First span of the run, whose column and marker the candidate must match. Must
 *                     not be `NULL`.
 * @return `true` when the span continues the run.
 */
static bool is_line_run_continuation(const char* source,
                                     const struct CommentSpanList* spans,
                                     size_t index,
                                     size_t previous_end,
                                     const struct CommentSpan* first)
    __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Returns the opener marker of `span`.
 *
 * @param source Source bytes. Must not be `NULL`.
 * @param span   Comment span. Must not be `NULL`.
 * @return `//`, `///`, or `//!` for a line comment, or slash-star, slash-star-star, or
 *         slash-star-bang for a block comment, in static storage.
 */
static const char* span_opener(const char* source, const struct CommentSpan* span)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether `block` holds any byte of prose between its markers.
 *
 * @param source Source bytes. Must not be `NULL`.
 * @param block  Grouped block. Must not be `NULL`.
 * @return `true` when an ASCII letter or digit, or a non-ASCII byte, appears in the block, or
 *         `false` for a banner or other decoration-only block that is copied unchanged.
 */
static bool has_alphanumeric_payload(const char* source, const struct CommentBlock* block)
    __attribute__((nonnull(1, 2)));

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
 * @brief Classifies a stripped payload as prose, tag, list, code, decoration, directive, or blank.
 *
 * @param text       Terminated payload. Must not be `NULL`.
 * @param text_len   Length of `text`.
 * @param is_doxygen Whether the block recognizes Doxygen commands.
 * @param state      Fence and tag state carried from the previous line, updated for the next one.
 *                   Must not be `NULL`.
 * @return Line kind.
 */
static enum BodyLineKind classify_payload(const char* text,
                                          size_t text_len,
                                          bool is_doxygen,
                                          struct PayloadState* state)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Returns the fence that the Doxygen command at the start of `text` opens.
 *
 * @param text     Payload bytes with leading whitespace removed. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @return `PAYLOAD_FENCE_CODE` for `@code`, `PAYLOAD_FENCE_VERBATIM` for `@verbatim`, or
 *         `PAYLOAD_FENCE_NONE` otherwise. The `\` spellings match as well.
 */
static enum PayloadFence fence_opened_by_command(const char* text, size_t text_len)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` starts with the Doxygen command that closes `fence`.
 *
 * @param text     Payload bytes with leading whitespace removed. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param fence    Open Doxygen fence.
 * @return `true` for `@endcode` in a code fence or `@endverbatim` in a verbatim fence.
 */
static bool is_doxygen_fence_closer(const char* text, size_t text_len, enum PayloadFence fence)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` starts with the Doxygen command `command`.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param command  Terminated command word without its marker. Must not be `NULL`.
 * @return `true` when the parsed command word equals `command`.
 */
static bool has_doxygen_command(const char* text, size_t text_len, const char* command)
    __attribute__((nonnull(1, 3)));

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
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @return Byte length of the leading whitespace.
 */
static size_t leading_space_len(const char* text, size_t text_len) __attribute__((nonnull(1)));

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
  // Each block holds at least one span, so the span count bounds the block count.
  struct CommentBlock* items = arena_calloc(arena, spans->count, sizeof(*items));
  if (items == NULL) {
    return error_report(err, err_len, "out of memory");
  }

  size_t n = 0;
  for (size_t i = 0; i < spans->count; i++) {
    const struct CommentSpan* span = &spans->items[i];
    if (!is_span_rewritable(source, span)) {
      continue;
    }
    struct CommentBlock* block = &items[n];
    fill_block_from_span(block, source, source_len, span);
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

  blocks_out->items = n > 0 ? items : NULL;
  blocks_out->count = n;
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
  struct PayloadState state = {.fence = PAYLOAD_FENCE_NONE, .is_in_tag = false};
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
      struct BodyLine* line = &items[n];
      line->kind = classify_payload(line->text, line->text_len, block->is_doxygen, &state);
      // Only a tag continuation is prose with an indent, which is not part of its description.
      if (line->kind == BODY_LINE_PROSE) {
        const size_t indent_len = leading_space_len(line->text, line->text_len);
        line->text += indent_len;
        line->text_len -= indent_len;
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
      if (string_buffer_append(buffer, block->opener) != 0) {
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

  if (string_buffer_append(buffer, block->opener) != 0) {
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
    // A lone closer below a one-line body is a shape this function never reproduces from its own
    // output, which would make `--check` report a rewrap for a file `--in-place` just wrote.
    if (comment_has_opener_closer(block, lines)) {
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
    // A continuation starts in the column after the opener and its space, which extraction strips.
    if (string_buffer_append_len(buffer, indent, indent_len) != 0 ||
        emit_indent(buffer, strlen(block->opener) + 1) != 0 ||
        string_buffer_append_len(buffer, lines->items[i].text, lines->items[i].text_len) != 0) {
      return -1;
    }
  }
  if (string_buffer_append(buffer, " */") != 0) {
    return -1;
  }
  return 0;
}

bool comment_has_opener_closer(const struct CommentBlock* block, const struct BodyLineList* lines) {
  return block->shape == COMMENT_SHAPE_STARRED && !block->is_opener_empty && lines->count == 1;
}

size_t comment_prefix_columns(const struct CommentBlock* block) {
  const size_t marker_columns =
      block->shape == COMMENT_SHAPE_STARRED ? strlen(" * ") : strlen(block->opener) + 1;
  return block->indent_columns + marker_columns;
}

size_t comment_first_prefix_columns(const struct CommentBlock* block) {
  if (block->is_opener_empty) {
    return comment_prefix_columns(block);
  }
  return block->indent_columns + strlen(block->opener) + 1;
}

bool comment_is_list_line(const char* text) {
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

bool comment_is_tool_directive(const char* text) {
  for (size_t i = 0; i < sizeof(tool_directives) / sizeof(tool_directives[0]); i++) {
    const char* const directive = tool_directives[i];
    const size_t directive_len = strlen(directive);
    if (strncmp(text, directive, directive_len) != 0) {
      continue;
    }
    const unsigned char last = (unsigned char)directive[directive_len - 1];
    const unsigned char next = (unsigned char)text[directive_len];
    if (!is_word_byte(last) || !is_word_byte(next)) {
      return true;
    }
  }
  return false;
}

static bool is_span_rewritable(const char* source, const struct CommentSpan* span) {
  if (span->is_trailing) {
    return false;
  }
  return span->kind != COMMENT_KIND_LINE ||
         memchr(source + span->start, '\n', span->end - span->start) == NULL;
}

static bool is_line_run_continuation(const char* source,
                                     const struct CommentSpanList* spans,
                                     size_t index,
                                     size_t previous_end,
                                     const struct CommentSpan* first) {
  if (index >= spans->count) {
    return false;
  }
  const struct CommentSpan* next = &spans->items[index];
  if (next->kind != COMMENT_KIND_LINE || !is_span_rewritable(source, next) ||
      next->opener_column != first->opener_column ||
      strcmp(span_opener(source, next), span_opener(source, first)) != 0) {
    return false;
  }
  // Only the line break and the next line's indent may separate the two spans. The column check
  // above keeps that indent at the run's own.
  size_t i = previous_end;
  if (source[i] == '\r') {
    i++;
  }
  if (source[i] != '\n') {
    return false;
  }
  i++;
  while (i < next->start && (source[i] == ' ' || source[i] == '\t')) {
    i++;
  }
  return i == next->start;
}

static const char* span_opener(const char* source, const struct CommentSpan* span) {
  const char* const text = source + span->start;
  const size_t text_len = span->end - span->start;
  if (span->kind == COMMENT_KIND_LINE) {
    if (text_len >= 3 && text[2] == '!') {
      return "//!";
    }
    // Four or more slashes are a plain comment whose payload starts with slashes.
    if (text_len >= 3 && text[2] == '/' && (text_len == 3 || text[3] != '/')) {
      return "///";
    }
    return "//";
  }
  // A slash-star-star that the closing slash follows is an empty plain block, not a Doxygen one.
  if (text_len >= 4 && text[2] == '*' && text[3] != '/') {
    return "/**";
  }
  if (text_len >= 3 && text[2] == '!') {
    return "/*!";
  }
  return "/*";
}

static bool has_alphanumeric_payload(const char* source, const struct CommentBlock* block) {
  for (size_t i = block->start; i < block->end; i++) {
    const unsigned char c = (unsigned char)source[i];
    if (c >= 0x80 || ascii_is_alphanumeric(c)) {
      return true;
    }
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
      column = lex_advance_column(column, (unsigned char)source[i]);
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
  block->opener = span_opener(source, span);
  block->is_doxygen = strcmp(block->opener, "//") != 0 && strcmp(block->opener, "/*") != 0;
  block->is_opener_empty = false;
  if (span->kind == COMMENT_KIND_LINE) {
    block->shape = COMMENT_SHAPE_LINE;
    return;
  }
  block->shape = classify_block_shape(source, span);
  size_t i = span->start + strlen(block->opener);
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
    const size_t opener_len = strlen(block->opener);
    if (end - start >= opener_len && memcmp(source + start, block->opener, opener_len) == 0) {
      start += opener_len;
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
      const size_t opener_len = strlen(block->opener);
      if (end - start >= opener_len && memcmp(source + start, block->opener, opener_len) == 0) {
        start += opener_len;
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
      start = skip_hanging_indent(source, start, end, comment_prefix_columns(block));
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
                                          bool is_doxygen,
                                          struct PayloadState* state) {
  const bool is_in_tag = state->is_in_tag;
  state->is_in_tag = false;
  if (text_len == 0) {
    return BODY_LINE_BLANK;
  }
  const size_t indent_len = leading_space_len(text, text_len);
  const char* const unindented = text + indent_len;
  const size_t unindented_len = text_len - indent_len;
  if (state->fence == PAYLOAD_FENCE_MARKDOWN) {
    if (is_fence_line(text)) {
      state->fence = PAYLOAD_FENCE_NONE;
    }
    return BODY_LINE_CODE;
  }
  if (state->fence != PAYLOAD_FENCE_NONE) {
    if (is_doxygen_fence_closer(unindented, unindented_len, state->fence)) {
      state->fence = PAYLOAD_FENCE_NONE;
    }
    return BODY_LINE_CODE;
  }
  if (is_fence_line(text)) {
    state->fence = PAYLOAD_FENCE_MARKDOWN;
    return BODY_LINE_CODE;
  }
  if (is_doxygen) {
    state->fence = fence_opened_by_command(unindented, unindented_len);
    if (state->fence != PAYLOAD_FENCE_NONE) {
      return BODY_LINE_CODE;
    }
  }
  if (comment_is_tool_directive(text)) {
    return BODY_LINE_DIRECTIVE;
  }
  // Extra indent after decoration marks a sample, unless the line continues a tag. A continuation
  // may sit at any depth, so a description refilled at another width still reads as one.
  if (indent_len > 0) {
    if (is_in_tag && !comment_is_list_line(unindented) &&
        !is_decoration_line(unindented, unindented_len)) {
      state->is_in_tag = true;
      return BODY_LINE_PROSE;
    }
    return BODY_LINE_CODE;
  }
  struct DoxygenTag tag;
  if (is_doxygen && doxygen_has_tag(text, text_len, &tag)) {
    state->is_in_tag = true;
    return BODY_LINE_TAG;
  }
  if (comment_is_list_line(text)) {
    return BODY_LINE_LIST;
  }
  if (is_decoration_line(text, text_len)) {
    return BODY_LINE_DECORATION;
  }
  state->is_in_tag = is_in_tag;
  return BODY_LINE_PROSE;
}

static enum PayloadFence fence_opened_by_command(const char* text, size_t text_len) {
  if (has_doxygen_command(text, text_len, "code")) {
    return PAYLOAD_FENCE_CODE;
  }
  if (has_doxygen_command(text, text_len, "verbatim")) {
    return PAYLOAD_FENCE_VERBATIM;
  }
  return PAYLOAD_FENCE_NONE;
}

static bool is_doxygen_fence_closer(const char* text, size_t text_len, enum PayloadFence fence) {
  const char* const closer = fence == PAYLOAD_FENCE_CODE ? "endcode" : "endverbatim";
  return has_doxygen_command(text, text_len, closer);
}

static bool has_doxygen_command(const char* text, size_t text_len, const char* command) {
  struct DoxygenTag tag;
  return doxygen_has_tag(text, text_len, &tag) && tag.command_len == strlen(command) &&
         memcmp(tag.command, command, tag.command_len) == 0;
}

static bool is_word_byte(unsigned char c) {
  return ascii_is_alphanumeric(c) || c == '_';
}

static size_t leading_space_len(const char* text, size_t text_len) {
  size_t len = 0;
  while (len < text_len && (text[len] == ' ' || text[len] == '\t')) {
    len++;
  }
  return len;
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
    columns = lex_advance_column(columns, (unsigned char)source[i]);
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
