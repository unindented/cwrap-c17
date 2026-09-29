#include "domain/fill.h"

#include <stdbool.h>
#include <string.h>

#include "core/error.h"
#include "domain/atom.h"
#include "domain/comment.h"
#include "domain/doxygen.h"
#include "shared/arena.h"

/** One prose paragraph of a body, joined and split into atoms before any line is filled. */
struct FillParagraph {
  /** Index of the first body line in the paragraph. */
  size_t start;

  /** Index immediately after the last body line in the paragraph. */
  size_t end;

  /** Terminated paragraph text joined with single spaces, which `atoms` alias. */
  const char* joined;

  /** Bytes at the start of `joined` that stay on the first line: a tag and any name. */
  size_t frozen_prefix_len;

  /** Display columns of the frozen prefix, where a wrapped description continues. */
  size_t tag_indent_columns;

  /** Atoms after the frozen prefix, with each line-opener atom merged into the one before it. */
  struct AtomList atoms;

  /** Display columns before the paragraph's first line. */
  size_t first_prefix_columns;

  /** Columns reserved after the last atom for a closer that trails it, or 0. */
  size_t closer_columns;
};

/**
 * @brief Reports whether `kind` may join a wrapable paragraph.
 *
 * @param kind Line kind.
 * @return `true` for prose, which includes the continuation lines of a tag, or `false` otherwise.
 */
static bool is_fillable(enum BodyLineKind kind);

/**
 * @brief Splits a paragraph of body lines into atoms before any of its lines is filled.
 *
 * Splitting every paragraph before filling any lets the refill count its lines exactly, so the
 * refilled list is allocated once.
 *
 * @param lines         Line list. Must not be `NULL`.
 * @param start         First paragraph line index, inclusive.
 * @param end           Last paragraph line index, exclusive. Must be greater than `start`.
 * @param arena         Arena that owns the joined text and atoms. Must not be `NULL`.
 * @param paragraph_out Receives the split paragraph. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int split_paragraph(const struct BodyLineList* lines,
                           size_t start,
                           size_t end,
                           struct Arena* arena,
                           struct FillParagraph* paragraph_out) __attribute__((nonnull(1, 4, 5)));

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
 * @brief Returns the atom index that ends the greedily filled line starting at `atom_index`.
 *
 * @param paragraph           Split paragraph. Must not be `NULL`.
 * @param atom_index          First atom on the line. Must be less than `paragraph->atoms.count`
 *                            unless the paragraph has no atoms.
 * @param line_prefix_columns Display columns before the line.
 * @param width               Wrapping column.
 * @return Index after the last atom on the line. The line takes at least one atom when any is left.
 */
static size_t filled_line_end(const struct FillParagraph* paragraph,
                              size_t atom_index,
                              size_t line_prefix_columns,
                              size_t width) __attribute__((nonnull(1)));

/**
 * @brief Returns the number of lines `append_filled_paragraph` builds from `paragraph`.
 *
 * @param paragraph      Split paragraph. Must not be `NULL`.
 * @param prefix_columns Continuation prefix columns.
 * @param width          Wrapping column.
 * @return Filled line count, at least 1.
 */
static size_t filled_line_count(const struct FillParagraph* paragraph,
                                size_t prefix_columns,
                                size_t width) __attribute__((nonnull(1)));

/**
 * @brief Appends greedily filled lines from one split paragraph.
 *
 * @param lines          Line list that receives the filled paragraph. Its array has room for every
 *                       line the paragraph fills. Must not be `NULL`.
 * @param paragraph      Split paragraph. Must not be `NULL`.
 * @param prefix_columns Continuation prefix columns.
 * @param width          Wrapping column.
 * @param arena          Arena that owns the new line texts. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_filled_paragraph(struct BodyLineList* lines,
                                   const struct FillParagraph* paragraph,
                                   size_t prefix_columns,
                                   size_t width,
                                   struct Arena* arena) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Appends one filled line: a lead followed by `atoms` joined with single spaces.
 *
 * @param lines      Line list that receives the line. Its array has room for it. Must not be
 *                   `NULL`.
 * @param kind       Kind assigned to the line.
 * @param lead       Bytes that open the line, such as a frozen tag prefix, or `NULL` for `lead_len`
 *                   spaces.
 * @param lead_len   Number of lead bytes.
 * @param atoms      Atoms placed after the lead. May be `NULL` only when `atom_count` is 0.
 * @param atom_count Number of atoms in `atoms`.
 * @param arena      Arena that owns the line text. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
static int append_filled_line(struct BodyLineList* lines,
                              enum BodyLineKind kind,
                              const char* lead,
                              size_t lead_len,
                              const struct Atom* atoms,
                              size_t atom_count,
                              struct Arena* arena) __attribute__((nonnull(1, 7)));

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

int fill_body_lines(struct BodyLineList* lines,
                    size_t first_prefix_columns,
                    size_t prefix_columns,
                    size_t width,
                    bool has_trailing_closer,
                    struct Arena* arena,
                    char* err,
                    size_t err_len) {
  // Each paragraph holds at least one line, so the line count bounds the paragraph count.
  struct FillParagraph* paragraphs = arena_calloc(arena, lines->count, sizeof(*paragraphs));
  if (paragraphs == NULL) {
    return error_report(err, err_len, "out of memory");
  }
  size_t paragraph_count = 0;
  // A copied line stays one line.
  size_t capacity = 0;
  size_t i = 0;
  while (i < lines->count) {
    if (!is_fillable(lines->items[i].kind) && lines->items[i].kind != BODY_LINE_TAG) {
      capacity++;
      i++;
      continue;
    }
    const size_t start = i;
    i++;
    while (i < lines->count && is_fillable(lines->items[i].kind)) {
      i++;
    }
    struct FillParagraph* paragraph = &paragraphs[paragraph_count];
    if (split_paragraph(lines, start, i, arena, paragraph) != 0) {
      return error_report(err, err_len, "out of memory");
    }
    // Only the body's first line follows the opener. The closer trails the last body line, so only
    // a paragraph that ends the body makes room for it.
    paragraph->first_prefix_columns = start == 0 ? first_prefix_columns : prefix_columns;
    paragraph->closer_columns = has_trailing_closer && i == lines->count ? 3 : 0;
    capacity += filled_line_count(paragraph, prefix_columns, width);
    paragraph_count++;
  }

  struct BodyLineList rebuilt = {
      .items = arena_calloc(arena, capacity, sizeof(*rebuilt.items)),
      .count = 0,
  };
  if (rebuilt.items == NULL) {
    return error_report(err, err_len, "out of memory");
  }
  size_t paragraph_index = 0;
  i = 0;
  while (i < lines->count) {
    if (paragraph_index == paragraph_count || paragraphs[paragraph_index].start != i) {
      rebuilt.items[rebuilt.count] = lines->items[i];
      rebuilt.count++;
      i++;
      continue;
    }
    const struct FillParagraph* paragraph = &paragraphs[paragraph_index];
    if (append_filled_paragraph(&rebuilt, paragraph, prefix_columns, width, arena) != 0) {
      return error_report(err, err_len, "out of memory");
    }
    i = paragraph->end;
    paragraph_index++;
  }
  lines->items = rebuilt.count > 0 ? rebuilt.items : NULL;
  lines->count = rebuilt.count;
  return 0;
}

static bool is_fillable(enum BodyLineKind kind) {
  return kind == BODY_LINE_PROSE;
}

static int split_paragraph(const struct BodyLineList* lines,
                           size_t start,
                           size_t end,
                           struct Arena* arena,
                           struct FillParagraph* paragraph_out) {
  size_t joined_len = 0;
  const char* joined = join_payloads(lines, start, end, arena, &joined_len);
  if (joined == NULL) {
    return -1;
  }
  const size_t frozen_prefix_len =
      lines->items[start].kind == BODY_LINE_TAG ? tag_prefix_len(joined, joined_len) : 0;
  struct AtomList atoms;
  if (atom_split(joined, joined_len, arena, &atoms) != 0) {
    return -1;
  }
  glue_line_opener_atoms(&atoms, joined, frozen_prefix_len);
  // The frozen prefix opens the first line, so the atoms inside it (the tag and name) are dropped
  // rather than written twice.
  size_t prefix_atom_count = 0;
  while (prefix_atom_count < atoms.count && (size_t)(atoms.items[prefix_atom_count].text - joined) +
                                                    atoms.items[prefix_atom_count].text_len <=
                                                frozen_prefix_len) {
    prefix_atom_count++;
  }
  *paragraph_out = (struct FillParagraph){
      .start = start,
      .end = end,
      .joined = joined,
      .frozen_prefix_len = frozen_prefix_len,
      .tag_indent_columns = atom_column_width(joined, frozen_prefix_len),
      .atoms = {.items = atoms.items + prefix_atom_count, .count = atoms.count - prefix_atom_count},
  };
  return 0;
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

static size_t filled_line_end(const struct FillParagraph* paragraph,
                              size_t atom_index,
                              size_t line_prefix_columns,
                              size_t width) {
  const struct AtomList* atoms = &paragraph->atoms;
  // The first atom on a line needs no gap: it follows the prefix, or sits at the description column
  // after a continuation indent.
  size_t used_columns = paragraph->tag_indent_columns;
  size_t line_end = atom_index;
  while (line_end < atoms->count) {
    const bool is_line_start = line_end == atom_index;
    const size_t gap = is_line_start ? 0 : 1;
    const bool is_last = line_end + 1 == atoms->count;
    const size_t extra_columns =
        gap + atoms->items[line_end].columns + (is_last ? paragraph->closer_columns : 0);
    if (!is_line_start &&
        is_width_exceeded(line_prefix_columns, used_columns, extra_columns, width)) {
      break;
    }
    used_columns += gap + atoms->items[line_end].columns;
    line_end++;
  }
  return line_end;
}

static size_t filled_line_count(const struct FillParagraph* paragraph,
                                size_t prefix_columns,
                                size_t width) {
  size_t line_count = 0;
  size_t atom_index = 0;
  do {
    const size_t line_prefix_columns =
        line_count == 0 ? paragraph->first_prefix_columns : prefix_columns;
    atom_index = filled_line_end(paragraph, atom_index, line_prefix_columns, width);
    line_count++;
  } while (atom_index < paragraph->atoms.count);
  return line_count;
}

static int append_filled_paragraph(struct BodyLineList* lines,
                                   const struct FillParagraph* paragraph,
                                   size_t prefix_columns,
                                   size_t width,
                                   struct Arena* arena) {
  // The frozen prefix opens the first line. Overflow lines indent to the same column so a wrapped
  // description stays under the first.
  size_t atom_index = 0;
  bool is_first_line = true;
  do {
    const size_t line_prefix_columns =
        is_first_line ? paragraph->first_prefix_columns : prefix_columns;
    const size_t line_end = filled_line_end(paragraph, atom_index, line_prefix_columns, width);
    const bool is_tag_line = is_first_line && paragraph->frozen_prefix_len > 0;
    const char* const lead = is_first_line ? paragraph->joined : NULL;
    const size_t lead_len =
        is_first_line ? paragraph->frozen_prefix_len : paragraph->tag_indent_columns;
    if (append_filled_line(lines, is_tag_line ? BODY_LINE_TAG : BODY_LINE_PROSE, lead, lead_len,
                           paragraph->atoms.items + atom_index, line_end - atom_index,
                           arena) != 0) {
      return -1;
    }
    atom_index = line_end;
    is_first_line = false;
  } while (atom_index < paragraph->atoms.count);
  return 0;
}

static int append_filled_line(struct BodyLineList* lines,
                              enum BodyLineKind kind,
                              const char* lead,
                              size_t lead_len,
                              const struct Atom* atoms,
                              size_t atom_count,
                              struct Arena* arena) {
  size_t text_len = lead_len;
  for (size_t i = 0; i < atom_count; i++) {
    text_len += (i > 0 ? 1 : 0) + atoms[i].text_len;
  }
  char* text = arena_alloc(arena, text_len + 1);
  if (text == NULL) {
    return -1;
  }

  if (lead == NULL) {
    memset(text, ' ', lead_len);
  } else {
    memcpy(text, lead, lead_len);
  }
  size_t offset = lead_len;
  for (size_t i = 0; i < atom_count; i++) {
    if (i > 0) {
      text[offset] = ' ';
      offset++;
    }
    memcpy(text + offset, atoms[i].text, atoms[i].text_len);
    offset += atoms[i].text_len;
  }
  text[offset] = '\0';

  lines->items[lines->count] = (struct BodyLine){.kind = kind, .text = text, .text_len = text_len};
  lines->count++;
  return 0;
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
