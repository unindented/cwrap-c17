#include "domain/doxygen.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "domain/atom.h"
#include "domain/comment.h"
#include "shared/arena.h"
#include "shared/str.h"

/**
 * @brief Returns the byte length of an option such as `[in]` or `{.c}` at `text.data[index]`.
 *
 * @param text  Payload bytes.
 * @param index Offset just past the command word.
 * @return Length through the closing bracket or brace, or 0 when no option closes before whitespace
 *         or the end of `text`.
 */
static ptrdiff_t option_len(struct Str text, ptrdiff_t index);

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
static ptrdiff_t name_columns_max(const struct BodyLineList* lines, const struct DoxygenTag* tag)
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
 */
static void align_named_line(struct BodyLine* line,
                             const struct DoxygenTag* tag,
                             ptrdiff_t name_columns_max,
                             bool is_continued,
                             struct Arena* arena) __attribute__((nonnull(1, 2, 5)));

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

bool doxygen_has_tag(struct Str text, struct DoxygenTag* tag_out) {
  *tag_out = (struct DoxygenTag){0};
  const char* const bytes = text.data;
  if (text.len < 2 || (bytes[0] != '@' && bytes[0] != '\\') || !is_command_letter(bytes[1])) {
    return false;
  }

  ptrdiff_t offset = 1;
  while (offset < text.len && is_command_letter(bytes[offset])) {
    offset++;
  }
  struct DoxygenTag tag = {.command = {bytes + 1, offset - 1}};
  offset += option_len(text, offset);
  if (offset < text.len && !is_tag_space(bytes[offset])) {
    return false;
  }
  tag.keyword_len = offset;
  while (offset < text.len && is_tag_space(bytes[offset])) {
    offset++;
  }

  if (is_named_command(&tag) && offset < text.len) {
    const ptrdiff_t name_start = offset;
    while (offset < text.len && !is_tag_space(bytes[offset])) {
      offset++;
    }
    tag.name = (struct Str){bytes + name_start, offset - name_start};
    while (offset < text.len && is_tag_space(bytes[offset])) {
      offset++;
    }
  }
  tag.description_offset = offset;
  *tag_out = tag;
  return true;
}

void doxygen_align_parameters(struct BodyLineList* lines, struct Arena* arena) {
  // Aligning a line keeps its name, so the widest name among one command's lines does not change
  // while they are aligned. It is measured again only when the command changes, not for each line.
  struct Str command = {0};
  ptrdiff_t columns_max = 0;
  for (ptrdiff_t i = 0; i < lines->count; i++) {
    struct BodyLine* line = &lines->items[i];
    struct DoxygenTag tag;
    if (line->kind != BODY_LINE_TAG || !doxygen_has_tag(line->text, &tag) || tag.name.len == 0) {
      continue;
    }
    if (!str_eq(tag.command, command)) {
      command = tag.command;
      columns_max = name_columns_max(lines, &tag);
    }
    const bool is_continued = i + 1 < lines->count && lines->items[i + 1].kind == BODY_LINE_PROSE;
    align_named_line(line, &tag, columns_max, is_continued, arena);
  }
}

static ptrdiff_t option_len(struct Str text, ptrdiff_t index) {
  if (index >= text.len || (text.data[index] != '[' && text.data[index] != '{')) {
    return 0;
  }
  const char closer = text.data[index] == '[' ? ']' : '}';
  for (ptrdiff_t i = index + 1; i < text.len && !is_tag_space(text.data[i]); i++) {
    if (text.data[i] == closer) {
      return i + 1 - index;
    }
  }
  return 0;
}

static bool is_named_command(const struct DoxygenTag* tag) {
  static const struct Str named_commands[] = {STR_INIT("param"), STR_INIT("retval")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(named_commands) / sizeof(named_commands[0])); i++) {
    if (str_eq(tag->command, named_commands[i])) {
      return true;
    }
  }
  return false;
}

static ptrdiff_t name_columns_max(const struct BodyLineList* lines, const struct DoxygenTag* tag) {
  ptrdiff_t columns_max = 0;
  for (ptrdiff_t i = 0; i < lines->count; i++) {
    const struct BodyLine* line = &lines->items[i];
    struct DoxygenTag sibling;
    if (line->kind != BODY_LINE_TAG || !doxygen_has_tag(line->text, &sibling) ||
        sibling.name.len == 0 || !str_eq(sibling.command, tag->command)) {
      continue;
    }
    const ptrdiff_t columns = atom_column_width(sibling.name);
    if (columns > columns_max) {
      columns_max = columns;
    }
  }
  return columns_max;
}

static void align_named_line(struct BodyLine* line,
                             const struct DoxygenTag* tag,
                             ptrdiff_t name_columns_max,
                             bool is_continued,
                             struct Arena* arena) {
  const bool has_description = tag->description_offset < line->text.len;
  // The fill joins a continuation with one space, which stands in for the separator here.
  const ptrdiff_t name_columns = atom_column_width(tag->name);
  const ptrdiff_t padding_len = (has_description || is_continued) && name_columns < name_columns_max
                                    ? name_columns_max - name_columns
                                    : 0;
  const ptrdiff_t space_len = padding_len + (has_description ? 1 : 0);

  // The line is the keyword, a space, the name, then spaces up to the description. It is measured
  // before it is built, so it takes one allocation of its exact length, as `append_filled_line`
  // explains.
  const struct Str keyword = str_head(line->text, tag->keyword_len);
  const struct Str description = str_tail(line->text, tag->description_offset);
  const ptrdiff_t len = keyword.len + 1 + tag->name.len + space_len + description.len;
  char* text = arena_new(arena, len, char);
  memcpy(text, keyword.data, (size_t)keyword.len);
  ptrdiff_t offset = keyword.len;
  text[offset] = ' ';
  offset++;
  memcpy(text + offset, tag->name.data, (size_t)tag->name.len);
  offset += tag->name.len;
  memset(text + offset, ' ', (size_t)space_len);
  offset += space_len;
  // `memcpy` requires valid pointers even for zero bytes, and an empty description points one past
  // the line's last byte.
  if (description.len > 0) {
    memcpy(text + offset, description.data, (size_t)description.len);
  }
  line->text = (struct Str){text, len};
}

static bool is_command_letter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_tag_space(char c) {
  return c == ' ' || c == '\t';
}
