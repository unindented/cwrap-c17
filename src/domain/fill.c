#include "domain/fill.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/error.h"
#include "domain/atom.h"
#include "domain/comment.h"
#include "domain/doxygen.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

/**
 * @brief Reports whether `kind` may join a wrapable paragraph.
 *
 * @param kind Line kind.
 * @return `true` for prose, which includes the continuation lines of a tag, or `false` otherwise.
 */
static bool is_fillable(enum BodyLineKind kind);

/**
 * @brief Joins `lines[start, end)` payloads with single spaces into arena-owned text.
 *
 * @param lines          Line list. Must not be `NULL`.
 * @param start          First line index, inclusive. Must be no greater than `end`.
 * @param end            Last line index, exclusive. Must be no greater than `lines->count`.
 * @param arena          Arena that owns the joined string. Must not be `NULL`.
 * @param joined_len_out Receives the joined byte length. Must not be `NULL`.
 * @return Joined text, or `NULL` on allocation failure.
 */
static char* join_payloads(const struct BodyLineList* lines,
                           size_t start,
                           size_t end,
                           struct Arena* arena,
                           size_t* joined_len_out) __attribute__((nonnull(1, 4, 5)));

/**
 * @brief Reports whether `join_payloads` puts a space before `lines[index]`.
 *
 * @param lines Line list. Must not be `NULL`.
 * @param start First joined line index.
 * @param index Line index to test. Must be less than `lines->count`.
 * @return `true` when both this line and the one before it in the join have payload.
 */
static bool has_join_space(const struct BodyLineList* lines, size_t start, size_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Appends one body line to a growable temporary list.
 *
 * @param lines    List that receives the line. Must not be `NULL`.
 * @param capacity Current capacity of `lines`; receives any grown capacity. Must not be `NULL`.
 * @param line     Line to append.
 * @param arena    Arena that owns any grown array. Must not be `NULL`.
 * @param err      Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len  Size of `err` in bytes.
 * @return `0` on success, or `-1` on overflow or allocation failure.
 */
static int append_line(struct BodyLineList* lines,
                       size_t* capacity,
                       struct BodyLine line,
                       struct Arena* arena,
                       char* err,
                       size_t err_len) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Appends one copied line to a growable temporary body-line list.
 *
 * @param lines    List that receives the line. Must not be `NULL`.
 * @param capacity Current capacity of `lines`; receives any grown capacity. Must not be `NULL`.
 * @param kind     Kind assigned to the appended line.
 * @param line     Buffer whose bytes are copied. Must not be `NULL`.
 * @param arena    Arena that owns the copied text and any grown array. Must not be `NULL`.
 * @param err      Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len  Size of `err` in bytes.
 * @return `0` on success, or `-1` on overflow or allocation failure.
 */
static int append_filled_line(struct BodyLineList* lines,
                              size_t* capacity,
                              enum BodyLineKind kind,
                              const struct StringBuffer* line,
                              struct Arena* arena,
                              char* err,
                              size_t err_len) __attribute__((nonnull(1, 2, 4, 5)));

/**
 * @brief Appends greedily filled lines from one joined paragraph.
 *
 * @param lines                Line list that receives the filled paragraph. Must not be `NULL`.
 * @param capacity             Current capacity of `lines`; receives any grown capacity. Must not be
 *                             `NULL`.
 * @param joined               Joined paragraph text. Must not be `NULL`.
 * @param joined_len           Number of bytes in `joined`.
 * @param frozen_prefix_len    Bytes at the start of `joined` that must stay on the first line.
 * @param first_prefix_columns First body-line prefix columns.
 * @param prefix_columns       Continuation prefix columns.
 * @param width                Wrapping column.
 * @param has_trailing_closer  Whether to reserve a trailing closer on the last line.
 * @param arena                Arena that owns the new line texts and a temporary array. Must not be
 *                             `NULL`.
 * @param err                  Diagnostic buffer. May be `NULL` only when `err_len` is 0.
 * @param err_len              Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_filled_paragraph(struct BodyLineList* lines,
                                   size_t* capacity,
                                   const char* joined,
                                   size_t joined_len,
                                   size_t frozen_prefix_len,
                                   size_t first_prefix_columns,
                                   size_t prefix_columns,
                                   size_t width,
                                   bool has_trailing_closer,
                                   struct Arena* arena,
                                   char* err,
                                   size_t err_len) __attribute__((nonnull(1, 2, 3, 10)));

/**
 * @brief Merges each atom that would open a line of another kind into the atom before it.
 *
 * A continuation line that started with a Doxygen command or a list marker would read back as a new
 * tag or list item and stop the paragraph from refilling, so such an atom travels with the word
 * before it. An atom inside the frozen tag prefix takes nothing, because the atom after the prefix
 * always stays on the first line.
 *
 * @param atoms             Atoms of `joined`, merged in place. Must not be `NULL`.
 * @param joined            Terminated paragraph text the atoms alias. Must not be `NULL`.
 * @param frozen_prefix_len Bytes at the start of `joined` that stay on the first line.
 */
static void glue_line_opener_atoms(struct AtomList* atoms,
                                   const char* joined,
                                   size_t frozen_prefix_len) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether adding columns would exceed the wrapping width.
 *
 * @param prefix_columns Columns occupied before the payload.
 * @param used_columns   Payload columns already on the line.
 * @param extra_columns  Columns proposed for the line.
 * @param width          Wrapping column.
 * @return `true` when the combined columns exceed `width`, or `false` otherwise.
 */
static bool is_width_exceeded(size_t prefix_columns,
                              size_t used_columns,
                              size_t extra_columns,
                              size_t width);

/**
 * @brief Returns the length of a frozen Doxygen tag prefix at the start of `text`.
 *
 * @param text     Payload. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @return Byte length of the tag keyword plus any documented name and the following spaces, or 0.
 */
static size_t tag_prefix_len(const char* text, size_t text_len) __attribute__((nonnull(1)));

/**
 * @brief Writes `count` spaces into `buffer`.
 *
 * @param buffer Destination buffer. Must not be `NULL`.
 * @param count  Number of spaces to write.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_spaces(struct StringBuffer* buffer, size_t count) __attribute__((nonnull(1)));

int fill_body_lines(struct BodyLineList* lines,
                    size_t first_prefix_columns,
                    size_t prefix_columns,
                    size_t width,
                    bool has_trailing_closer,
                    struct Arena* arena,
                    char* err,
                    size_t err_len) {
  struct BodyLineList rebuilt = {0};
  size_t rebuilt_capacity = 0;
  size_t i = 0;
  while (i < lines->count) {
    if (!is_fillable(lines->items[i].kind) && lines->items[i].kind != BODY_LINE_TAG) {
      if (append_line(&rebuilt, &rebuilt_capacity, lines->items[i], arena, err, err_len) != 0) {
        return -1;
      }
      i++;
      continue;
    }
    const size_t start = i;
    const bool is_tag_start = lines->items[i].kind == BODY_LINE_TAG;
    i++;
    while (i < lines->count && is_fillable(lines->items[i].kind)) {
      i++;
    }
    size_t joined_len = 0;
    char* joined = join_payloads(lines, start, i, arena, &joined_len);
    if (joined == NULL) {
      return error_report(err, err_len, "out of memory");
    }
    const size_t frozen_prefix_len = is_tag_start ? tag_prefix_len(joined, joined_len) : 0;
    // The closer trails the last body line, so only a paragraph that ends the body makes room for
    // it.
    const bool is_closer_reserved = has_trailing_closer && i == lines->count;
    if (append_filled_paragraph(&rebuilt, &rebuilt_capacity, joined, joined_len, frozen_prefix_len,
                                first_prefix_columns, prefix_columns, width, is_closer_reserved,
                                arena, err, err_len) != 0) {
      return -1;
    }
  }
  *lines = rebuilt;
  return 0;
}

static bool is_fillable(enum BodyLineKind kind) {
  return kind == BODY_LINE_PROSE;
}

static char* join_payloads(const struct BodyLineList* lines,
                           size_t start,
                           size_t end,
                           struct Arena* arena,
                           size_t* joined_len_out) {
  size_t joined_len = 0;
  for (size_t i = start; i < end; i++) {
    joined_len += (has_join_space(lines, start, i) ? 1 : 0) + lines->items[i].text_len;
  }
  char* joined = arena_alloc(arena, joined_len + 1);
  if (joined == NULL) {
    return NULL;
  }

  size_t offset = 0;
  for (size_t i = start; i < end; i++) {
    if (has_join_space(lines, start, i)) {
      joined[offset] = ' ';
      offset++;
    }
    memcpy(joined + offset, lines->items[i].text, lines->items[i].text_len);
    offset += lines->items[i].text_len;
  }
  joined[offset] = '\0';
  *joined_len_out = joined_len;
  return joined;
}

static bool has_join_space(const struct BodyLineList* lines, size_t start, size_t index) {
  return index > start && lines->items[index - 1].text_len > 0 && lines->items[index].text_len > 0;
}

static int append_line(struct BodyLineList* lines,
                       size_t* capacity,
                       struct BodyLine line,
                       struct Arena* arena,
                       char* err,
                       size_t err_len) {
  if (lines->count == *capacity) {
    if (*capacity > SIZE_MAX / 2) {
      return error_report(err, err_len, "body line count exceeds max supported count (%zu) at %zu",
                          SIZE_MAX / 2, *capacity);
    }
    const size_t capacity_next = *capacity == 0 ? 4 : *capacity * 2;
    struct BodyLine* grown = arena_calloc(arena, capacity_next, sizeof(*grown));
    if (grown == NULL) {
      return error_report(err, err_len, "out of memory");
    }
    if (lines->items != NULL) {
      memcpy(grown, lines->items, lines->count * sizeof(*grown));
    }
    lines->items = grown;
    *capacity = capacity_next;
  }
  lines->items[lines->count] = line;
  lines->count++;
  return 0;
}

static int append_filled_line(struct BodyLineList* lines,
                              size_t* capacity,
                              enum BodyLineKind kind,
                              const struct StringBuffer* line,
                              struct Arena* arena,
                              char* err,
                              size_t err_len) {
  char* text = arena_strndup(arena, line->data == NULL ? "" : line->data, line->len);
  if (text == NULL) {
    return error_report(err, err_len, "out of memory");
  }
  const struct BodyLine filled_line = {
      .kind = kind,
      .text = text,
      .text_len = line->len,
  };
  return append_line(lines, capacity, filled_line, arena, err, err_len);
}

static int append_filled_paragraph(struct BodyLineList* lines,
                                   size_t* capacity,
                                   const char* joined,
                                   size_t joined_len,
                                   size_t frozen_prefix_len,
                                   size_t first_prefix_columns,
                                   size_t prefix_columns,
                                   size_t width,
                                   bool has_trailing_closer,
                                   struct Arena* arena,
                                   char* err,
                                   size_t err_len) {
  struct AtomList atoms;
  if (atom_split(joined, joined_len, arena, &atoms) != 0) {
    return error_report(err, err_len, "out of memory");
  }
  glue_line_opener_atoms(&atoms, joined, frozen_prefix_len);

  const size_t closer_columns = has_trailing_closer ? 3 : 0;

  struct StringBuffer line;
  string_buffer_init(&line);
  int rc = -1;
  enum BodyLineKind final_kind = BODY_LINE_PROSE;

  size_t atom_index = 0;
  // A frozen tag prefix is emitted on the first line before atoms that follow it. Atoms that sit
  // inside the prefix (the tag and name) are skipped so they are not duplicated. Overflow lines
  // indent to the same column so a wrapped description stays under the first.
  size_t skip_until = frozen_prefix_len;
  bool is_first_line = true;
  const size_t tag_indent_columns =
      frozen_prefix_len > 0 ? atom_column_width(joined, frozen_prefix_len) : 0;
  size_t used_columns = tag_indent_columns;
  size_t line_prefix_columns = lines->count == 0 ? first_prefix_columns : prefix_columns;

  if (frozen_prefix_len > 0) {
    if (string_buffer_append_len(&line, joined, frozen_prefix_len) != 0) {
      (void)error_report(err, err_len, "out of memory");
      goto cleanup;
    }
  }

  while (atom_index < atoms.count) {
    const struct Atom* atom = &atoms.items[atom_index];
    const size_t atom_offset = (size_t)(atom->text - joined);
    if (atom_offset + atom->text_len <= skip_until) {
      atom_index++;
      continue;
    }

    const bool is_last = atom_index + 1 == atoms.count;
    // After a frozen prefix or a continuation indent, the next atom is already at the description
    // column and needs no extra space.
    const bool is_at_tag_column = tag_indent_columns > 0 && used_columns == tag_indent_columns;
    const size_t gap = used_columns > 0 && !is_at_tag_column ? 1 : 0;

    const size_t extra_columns = gap + atom->columns + (is_last ? closer_columns : 0);
    if (used_columns > tag_indent_columns &&
        is_width_exceeded(line_prefix_columns, used_columns, extra_columns, width)) {
      const enum BodyLineKind kind =
          is_first_line && frozen_prefix_len > 0 ? BODY_LINE_TAG : BODY_LINE_PROSE;
      if (append_filled_line(lines, capacity, kind, &line, arena, err, err_len) != 0) {
        goto cleanup;
      }
      string_buffer_free(&line);
      string_buffer_init(&line);
      is_first_line = false;
      skip_until = 0;
      if (tag_indent_columns > 0 && append_spaces(&line, tag_indent_columns) != 0) {
        (void)error_report(err, err_len, "out of memory");
        goto cleanup;
      }
      used_columns = tag_indent_columns;
      line_prefix_columns = prefix_columns;
      continue;
    }

    if (gap > 0 && string_buffer_append_char(&line, ' ') != 0) {
      (void)error_report(err, err_len, "out of memory");
      goto cleanup;
    }
    if (string_buffer_append_len(&line, atom->text, atom->text_len) != 0) {
      (void)error_report(err, err_len, "out of memory");
      goto cleanup;
    }
    used_columns += gap + atom->columns;
    atom_index++;
  }

  final_kind = is_first_line && frozen_prefix_len > 0 ? BODY_LINE_TAG : BODY_LINE_PROSE;
  if (append_filled_line(lines, capacity, final_kind, &line, arena, err, err_len) != 0) {
    goto cleanup;
  }
  rc = 0;

cleanup:
  string_buffer_free(&line);
  return rc;
}

static void glue_line_opener_atoms(struct AtomList* atoms,
                                   const char* joined,
                                   size_t frozen_prefix_len) {
  size_t kept = 0;
  for (size_t i = 0; i < atoms->count; i++) {
    const struct Atom atom = atoms->items[i];
    struct DoxygenTag tag;
    // Each atom is followed in `joined` by the space or end that follows it on an emitted line.
    const bool is_line_opener =
        doxygen_has_tag(atom.text, atom.text_len, &tag) || comment_is_list_line(atom.text);
    if (kept > 0 && is_line_opener) {
      struct Atom* previous = &atoms->items[kept - 1];
      if ((size_t)(previous->text - joined) + previous->text_len > frozen_prefix_len) {
        previous->text_len = (size_t)(atom.text + atom.text_len - previous->text);
        previous->columns = atom_column_width(previous->text, previous->text_len);
        continue;
      }
    }
    atoms->items[kept] = atom;
    kept++;
  }
  atoms->count = kept;
}

static bool is_width_exceeded(size_t prefix_columns,
                              size_t used_columns,
                              size_t extra_columns,
                              size_t width) {
  if (prefix_columns > width) {
    return true;
  }
  const size_t payload_columns = width - prefix_columns;
  if (used_columns > payload_columns) {
    return true;
  }
  return extra_columns > payload_columns - used_columns;
}

static size_t tag_prefix_len(const char* text, size_t text_len) {
  struct DoxygenTag tag;
  return doxygen_has_tag(text, text_len, &tag) ? tag.description_offset : 0;
}

static int append_spaces(struct StringBuffer* buffer, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (string_buffer_append_char(buffer, ' ') != 0) {
      return -1;
    }
  }
  return 0;
}
