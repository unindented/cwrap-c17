#include "domain/doxygen.h"

#include <stdbool.h>
#include <string.h>

#include "core/arena.h"
#include "core/error.h"
#include "core/string_buffer.h"
#include "domain/atom.h"
#include "domain/comment.h"

/**
 * @brief Rebuilds one `@param` line with its description aligned.
 *
 * @param line                       Line to rebuild. Must not be `NULL`.
 * @param tag                        Parsed parameter tag within `line->text`. Must not be `NULL`.
 * @param parameter_name_columns_max Longest parameter-name width in the body.
 * @param arena                      Arena that owns the replacement text. Must not be `NULL`.
 * @param err                        Receives a diagnostic on failure. May be `NULL` only when
 *                               `err_len` is 0.
 * @param err_len                    Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int align_parameter_line(struct BodyLine* line,
                                const struct DoxygenTag* tag,
                                size_t parameter_name_columns_max,
                                struct Arena* arena,
                                char* err,
                                size_t err_len) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Rebuilds one continuation at its tag's description column.
 *
 * @param line               Line to rebuild. Must not be `NULL`.
 * @param description_column Column at which its text begins.
 * @param leading_len        Existing leading-space count to replace.
 * @param arena              Arena that owns the replacement text. Must not be `NULL`.
 * @param err                Receives a diagnostic on failure. May be `NULL` only when `err_len` is
 *                           0.
 * @param err_len            Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int align_continuation_line(struct BodyLine* line,
                                   size_t description_column,
                                   size_t leading_len,
                                   struct Arena* arena,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1, 4)));

/**
 * @brief Reports whether `c` separates parts of a Doxygen tag.
 *
 * @param c Byte to classify.
 * @return `true` for a space or tab, or `false` otherwise.
 */
static bool is_tag_space(char c);

/**
 * @brief Returns the payload column where a tag's description begins.
 *
 * @param tag          Parsed tag. Must not be `NULL`.
 * @param name_padding Columns to reserve for the parameter name, including following space.
 * @return Description start column.
 */
static size_t description_column(const struct DoxygenTag* tag, size_t name_padding)
    __attribute__((nonnull(1)));

bool doxygen_has_tag(const char* text, struct DoxygenTag* tag_out) {
  *tag_out = (struct DoxygenTag){.kind = DOXYGEN_TAG_NONE};

  size_t keyword_len = 0;
  enum DoxygenTagKind kind = DOXYGEN_TAG_NONE;
  if (strncmp(text, "@brief", 6) == 0) {
    keyword_len = 6;
    kind = DOXYGEN_TAG_BRIEF;
  } else if (strncmp(text, "@return", 7) == 0) {
    keyword_len = 7;
    kind = DOXYGEN_TAG_RETURN;
  } else if (strncmp(text, "@param", 6) == 0) {
    keyword_len = 6;
    kind = DOXYGEN_TAG_PARAM;
    if (strncmp(text + keyword_len, "[in,out]", 8) == 0) {
      keyword_len += 8;
    } else if (strncmp(text + keyword_len, "[out]", 5) == 0) {
      keyword_len += 5;
    } else if (strncmp(text + keyword_len, "[in]", 4) == 0) {
      keyword_len += 4;
    }
  } else {
    return false;
  }

  if (text[keyword_len] != '\0' && !is_tag_space(text[keyword_len])) {
    return false;
  }

  size_t offset = keyword_len;
  while (is_tag_space(text[offset])) {
    offset++;
  }

  const char* parameter_name = NULL;
  size_t parameter_name_len = 0;
  if (kind == DOXYGEN_TAG_PARAM && text[offset] != '\0') {
    parameter_name = text + offset;
    while (text[offset] != '\0' && !is_tag_space(text[offset])) {
      offset++;
      parameter_name_len++;
    }
    while (is_tag_space(text[offset])) {
      offset++;
    }
  }

  *tag_out = (struct DoxygenTag){
      .kind = kind,
      .keyword_len = keyword_len,
      .parameter_name = parameter_name,
      .parameter_name_len = parameter_name_len,
      .description_offset = offset,
  };
  return true;
}

int doxygen_align_parameters(struct BodyLineList* lines,
                             struct Arena* arena,
                             char* err,
                             size_t err_len) {
  size_t parameter_name_columns_max = 0;
  for (size_t i = 0; i < lines->count; i++) {
    if (lines->items[i].kind != BODY_LINE_TAG) {
      continue;
    }
    struct DoxygenTag tag;
    if (doxygen_has_tag(lines->items[i].text, &tag) && tag.kind == DOXYGEN_TAG_PARAM) {
      const size_t parameter_name_columns =
          atom_column_width(tag.parameter_name, tag.parameter_name_len);
      if (parameter_name_columns > parameter_name_columns_max) {
        parameter_name_columns_max = parameter_name_columns;
      }
    }
  }

  size_t current_description_column = 0;
  for (size_t i = 0; i < lines->count; i++) {
    struct BodyLine* line = &lines->items[i];
    if (line->kind == BODY_LINE_TAG) {
      struct DoxygenTag tag;
      if (!doxygen_has_tag(line->text, &tag)) {
        current_description_column = 0;
        continue;
      }
      const bool is_parameter = tag.kind == DOXYGEN_TAG_PARAM && tag.parameter_name_len > 0;
      const size_t name_padding = is_parameter ? parameter_name_columns_max + 1 : 0;
      current_description_column = description_column(&tag, name_padding);
      if (!is_parameter) {
        continue;
      }
      if (align_parameter_line(line, &tag, parameter_name_columns_max, arena, err, err_len) != 0) {
        return -1;
      }
      continue;
    }

    if (line->kind == BODY_LINE_PROSE && current_description_column > 0) {
      size_t leading_len = 0;
      while (leading_len < line->text_len && line->text[leading_len] == ' ') {
        leading_len++;
      }
      if (leading_len == current_description_column) {
        continue;
      }
      if (align_continuation_line(line, current_description_column, leading_len, arena, err,
                                  err_len) != 0) {
        return -1;
      }
    } else if (line->kind != BODY_LINE_PROSE) {
      current_description_column = 0;
    }
  }
  return 0;
}

static int align_parameter_line(struct BodyLine* line,
                                const struct DoxygenTag* tag,
                                size_t parameter_name_columns_max,
                                struct Arena* arena,
                                char* err,
                                size_t err_len) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  int rc = -1;
  const char* description = NULL;
  char* copy = NULL;

  if (string_buffer_append_len(&buffer, line->text, tag->keyword_len) != 0 ||
      string_buffer_append_char(&buffer, ' ') != 0 ||
      string_buffer_append_len(&buffer, tag->parameter_name, tag->parameter_name_len) != 0) {
    goto cleanup;
  }
  description = line->text + tag->description_offset;
  if (description[0] != '\0') {
    const size_t parameter_name_columns =
        atom_column_width(tag->parameter_name, tag->parameter_name_len);
    for (size_t padding = parameter_name_columns; padding < parameter_name_columns_max; padding++) {
      if (string_buffer_append_char(&buffer, ' ') != 0) {
        goto cleanup;
      }
    }
    if (string_buffer_append_char(&buffer, ' ') != 0 ||
        string_buffer_append(&buffer, description) != 0) {
      goto cleanup;
    }
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

static int align_continuation_line(struct BodyLine* line,
                                   size_t description_column,
                                   size_t leading_len,
                                   struct Arena* arena,
                                   char* err,
                                   size_t err_len) {
  struct StringBuffer buffer;
  string_buffer_init(&buffer);
  int rc = -1;
  char* copy = NULL;

  for (size_t column = 0; column < description_column; column++) {
    if (string_buffer_append_char(&buffer, ' ') != 0) {
      goto cleanup;
    }
  }
  if (string_buffer_append(&buffer, line->text + leading_len) != 0) {
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

static bool is_tag_space(char c) {
  return c == ' ' || c == '\t';
}

static size_t description_column(const struct DoxygenTag* tag, size_t name_padding) {
  if (tag->kind == DOXYGEN_TAG_PARAM && tag->parameter_name_len > 0) {
    return tag->keyword_len + 1 + name_padding;
  }
  return tag->description_offset;
}
