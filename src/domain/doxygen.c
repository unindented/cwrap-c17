#include "domain/doxygen.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "domain/atom.h"
#include "domain/comment.h"
#include "shared/arena.h"
#include "shared/str.h"

/** Commands that document a name, whose descriptions align with their siblings'. */
static const struct Str named_commands[] = {STR_INIT("param"), STR_INIT("retval")};

enum { NAMED_COMMAND_COUNT = sizeof(named_commands) / sizeof(named_commands[0]) };

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
 * @brief Returns the index of `command` in `named_commands`, or -1 when it documents no name.
 *
 * @param command Command word after the marker.
 * @return Index into `named_commands`, or -1.
 */
static ptrdiff_t named_command_index(struct Str command);

/**
 * @brief Parses `line` as a tag that documents a name.
 *
 * @param line    Body line. Must not be `NULL`.
 * @param tag_out Receives the parsed tag. Must not be `NULL`.
 * @return Index of the tag's command in `named_commands`, or -1 when `line` is not a tag with a
 *         name.
 */
static ptrdiff_t named_tag_index(const struct BodyLine* line, struct DoxygenTag* tag_out)
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

  if (named_command_index(tag.command) >= 0 && offset < text.len) {
    const ptrdiff_t name_start = offset;
    offset += atom_len(text, offset);
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
  // Aligning a line keeps its name, so each command's widest name is measured once, before any line
  // is aligned.
  ptrdiff_t columns_max[NAMED_COMMAND_COUNT] = {0};
  for (ptrdiff_t i = 0; i < lines->count; i++) {
    struct DoxygenTag tag;
    const ptrdiff_t k = named_tag_index(&lines->items[i], &tag);
    if (k >= 0 && atom_column_width(tag.name) > columns_max[k]) {
      columns_max[k] = atom_column_width(tag.name);
    }
  }
  for (ptrdiff_t i = 0; i < lines->count; i++) {
    struct DoxygenTag tag;
    const ptrdiff_t k = named_tag_index(&lines->items[i], &tag);
    if (k < 0) {
      continue;
    }
    const bool is_continued = i + 1 < lines->count && lines->items[i + 1].kind == BODY_LINE_PROSE;
    align_named_line(&lines->items[i], &tag, columns_max[k], is_continued, arena);
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

static ptrdiff_t named_command_index(struct Str command) {
  for (ptrdiff_t i = 0; i < NAMED_COMMAND_COUNT; i++) {
    if (str_eq(command, named_commands[i])) {
      return i;
    }
  }
  return -1;
}

static ptrdiff_t named_tag_index(const struct BodyLine* line, struct DoxygenTag* tag_out) {
  if (line->kind != BODY_LINE_TAG || !doxygen_has_tag(line->text, tag_out) ||
      tag_out->name.len == 0) {
    return -1;
  }
  return named_command_index(tag_out->command);
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
