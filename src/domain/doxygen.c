#include "domain/doxygen.h"

#include <stdbool.h>
#include <string.h>

#include "core/error.h"
#include "domain/atom.h"
#include "domain/comment.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

/**
 * @brief Returns the byte length of an option such as `[in]` or `{.c}` at `text[index]`.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Number of bytes in `text`.
 * @param index    Offset just past the command word.
 * @return Length through the closing bracket or brace, or 0 when no option closes before whitespace
 *         or the end of `text`.
 */
static size_t option_len(const char* text, size_t text_len, size_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `tag` documents a name whose description aligns with its siblings.
 *
 * @param tag Parsed tag. Must not be `NULL`.
 * @return `true` for a `param` or `retval` command, or `false` otherwise.
 */
static bool is_named_command(const struct DoxygenTag* tag) __attribute__((nonnull(1)));

/**
 * @brief Returns the widest name among the tags in `lines` that share the command of `tag`.
 *
 * @param lines Body lines to scan. Must not be `NULL`.
 * @param tag   Named tag whose command selects the siblings. Must not be `NULL`.
 * @return Display columns of the widest sibling name.
 */
static size_t name_columns_max(const struct BodyLineList* lines, const struct DoxygenTag* tag)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Rebuilds one named tag line with its description aligned.
 *
 * @param line             Line to rebuild. Must not be `NULL`.
 * @param tag              Parsed named tag within `line->text`. Must not be `NULL`.
 * @param name_columns_max Widest sibling name width in the body.
 * @param is_continued     Whether the next line continues the tag, so a description that starts
 *                         there still needs the padding before it.
 * @param arena            Arena that owns the replacement text. Must not be `NULL`.
 * @param err              Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len          Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int align_named_line(struct BodyLine* line,
                            const struct DoxygenTag* tag,
                            size_t name_columns_max,
                            bool is_continued,
                            struct Arena* arena,
                            char* err,
                            size_t err_len) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Reports whether `c` is an ASCII letter, the only byte a command word holds.
 *
 * @param c Byte to classify.
 * @return `true` for `[A-Za-z]`, or `false` otherwise.
 */
static bool is_command_letter(char c);

/**
 * @brief Reports whether `c` separates parts of a Doxygen tag.
 *
 * @param c Byte to classify.
 * @return `true` for a space or tab, or `false` otherwise.
 */
static bool is_tag_space(char c);

bool doxygen_has_tag(const char* text, size_t text_len, struct DoxygenTag* tag_out) {
  *tag_out = (struct DoxygenTag){0};
  if (text_len < 2 || (text[0] != '@' && text[0] != '\\') || !is_command_letter(text[1])) {
    return false;
  }

  size_t offset = 1;
  while (offset < text_len && is_command_letter(text[offset])) {
    offset++;
  }
  struct DoxygenTag tag = {.command = text + 1, .command_len = offset - 1};
  offset += option_len(text, text_len, offset);
  if (offset < text_len && !is_tag_space(text[offset])) {
    return false;
  }
  tag.keyword_len = offset;
  while (offset < text_len && is_tag_space(text[offset])) {
    offset++;
  }

  if (is_named_command(&tag) && offset < text_len) {
    const size_t name_start = offset;
    while (offset < text_len && !is_tag_space(text[offset])) {
      offset++;
    }
    tag.name = text + name_start;
    tag.name_len = offset - name_start;
    while (offset < text_len && is_tag_space(text[offset])) {
      offset++;
    }
  }
  tag.description_offset = offset;
  *tag_out = tag;
  return true;
}

int doxygen_align_parameters(struct BodyLineList* lines,
                             struct Arena* arena,
                             char* err,
                             size_t err_len) {
  for (size_t i = 0; i < lines->count; i++) {
    struct BodyLine* line = &lines->items[i];
    struct DoxygenTag tag;
    if (line->kind != BODY_LINE_TAG || !doxygen_has_tag(line->text, line->text_len, &tag) ||
        tag.name == NULL) {
      continue;
    }
    const bool is_continued = i + 1 < lines->count && lines->items[i + 1].kind == BODY_LINE_PROSE;
    if (align_named_line(line, &tag, name_columns_max(lines, &tag), is_continued, arena, err,
                         err_len) != 0) {
      return -1;
    }
  }
  return 0;
}

static size_t option_len(const char* text, size_t text_len, size_t index) {
  if (index >= text_len || (text[index] != '[' && text[index] != '{')) {
    return 0;
  }
  const char closer = text[index] == '[' ? ']' : '}';
  for (size_t i = index + 1; i < text_len && !is_tag_space(text[i]); i++) {
    if (text[i] == closer) {
      return i + 1 - index;
    }
  }
  return 0;
}

static bool is_named_command(const struct DoxygenTag* tag) {
  static const char* const named_commands[] = {"param", "retval"};
  for (size_t i = 0; i < sizeof(named_commands) / sizeof(named_commands[0]); i++) {
    if (strlen(named_commands[i]) == tag->command_len &&
        memcmp(tag->command, named_commands[i], tag->command_len) == 0) {
      return true;
    }
  }
  return false;
}

static size_t name_columns_max(const struct BodyLineList* lines, const struct DoxygenTag* tag) {
  size_t columns_max = 0;
  for (size_t i = 0; i < lines->count; i++) {
    const struct BodyLine* line = &lines->items[i];
    struct DoxygenTag sibling;
    if (line->kind != BODY_LINE_TAG || !doxygen_has_tag(line->text, line->text_len, &sibling) ||
        sibling.name == NULL || sibling.command_len != tag->command_len ||
        memcmp(sibling.command, tag->command, tag->command_len) != 0) {
      continue;
    }
    const size_t columns = atom_column_width(sibling.name, sibling.name_len);
    if (columns > columns_max) {
      columns_max = columns;
    }
  }
  return columns_max;
}

static int align_named_line(struct BodyLine* line,
                            const struct DoxygenTag* tag,
                            size_t name_columns_max,
                            bool is_continued,
                            struct Arena* arena,
                            char* err,
                            size_t err_len) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  int rc = -1;
  char* copy = NULL;
  const bool has_description = tag->description_offset < line->text_len;

  if (string_buffer_append_len(&buffer, line->text, tag->keyword_len) != 0 ||
      string_buffer_append_char(&buffer, ' ') != 0 ||
      string_buffer_append_len(&buffer, tag->name, tag->name_len) != 0) {
    goto cleanup;
  }
  // The fill joins a continuation with one space, which stands in for the separator here.
  if (has_description || is_continued) {
    const size_t name_columns = atom_column_width(tag->name, tag->name_len);
    for (size_t padding = name_columns; padding < name_columns_max; padding++) {
      if (string_buffer_append_char(&buffer, ' ') != 0) {
        goto cleanup;
      }
    }
  }
  if (has_description &&
      (string_buffer_append_char(&buffer, ' ') != 0 ||
       string_buffer_append_len(&buffer, line->text + tag->description_offset,
                                line->text_len - tag->description_offset) != 0)) {
    goto cleanup;
  }
  copy = arena_strndup(arena, buffer.data == NULL ? "" : buffer.data, buffer.len);
  if (copy == NULL) {
    goto cleanup;
  }
  line->text = copy;
  line->text_len = buffer.len;
  rc = 0;

cleanup:
  string_buffer_free(&buffer);
  return rc == 0 ? 0 : error_report(err, err_len, "out of memory");
}

static bool is_command_letter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_tag_space(char c) {
  return c == ' ' || c == '\t';
}
