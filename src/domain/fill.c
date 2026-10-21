#include "domain/fill.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "domain/atom.h"
#include "domain/comment.h"
#include "domain/doxygen.h"
#include "shared/arena.h"
#include "shared/str.h"

/** One prose paragraph of a body, joined and split into atoms before any line is filled. */
struct FillParagraph {
  /** Index of the first body line in the paragraph. */
  ptrdiff_t start;

  /** Index immediately after the last body line in the paragraph. */
  ptrdiff_t end;

  /** Paragraph text joined with single spaces, which `atoms` are views into. */
  struct Str joined;

  /** Bytes at the start of `joined` that stay on the first line: a tag and any name. */
  ptrdiff_t frozen_prefix_len;

  /** Display columns of the frozen prefix, where a wrapped description continues. */
  ptrdiff_t tag_indent_columns;

  /** Atoms after the frozen prefix, with each line-opener atom merged into the one before it. */
  struct AtomList atoms;

  /** Display columns before the paragraph's first line. */
  ptrdiff_t prefix_columns_first;

  /** Columns reserved after the last atom for a closer that trails it, or 0. */
  ptrdiff_t closer_columns;
};

/**
 * @brief Splits a paragraph of body lines into atoms before any of its lines is filled.
 *
 * Splitting every paragraph before filling any lets the refill count its lines exactly, so the
 * refilled list is allocated once.
 *
 * @param lines Line list. Must not be `NULL`.
 * @param start First paragraph line index, inclusive.
 * @param end   Last paragraph line index, exclusive. Must be greater than `start`.
 * @param arena Arena that owns the joined text and atoms. Must not be `NULL`.
 * @return The split paragraph, with its first-line prefix and closer columns still 0.
 */
static struct FillParagraph split_paragraph(const struct BodyLineList* lines,
                                            ptrdiff_t start,
                                            ptrdiff_t end,
                                            struct Arena* arena) __attribute__((nonnull(1, 4)));

/**
 * @brief Joins `lines[start, end)` payloads with single spaces into arena-owned text.
 *
 * @param lines Line list. Must not be `NULL`.
 * @param start First line index, inclusive. Must be no greater than `end`.
 * @param end   Last line index, exclusive. Must be no greater than `lines->count`.
 * @param arena Arena that owns the joined string. Must not be `NULL`.
 * @return Joined text.
 */
static struct Str join_payloads(const struct BodyLineList* lines,
                                ptrdiff_t start,
                                ptrdiff_t end,
                                struct Arena* arena) __attribute__((nonnull(1, 4)));

/**
 * @brief Reports whether `join_payloads` puts a space before `lines[index]`.
 *
 * @param lines Line list. Must not be `NULL`.
 * @param start First joined line index.
 * @param index Line index to test. Must be less than `lines->count`.
 * @return `true` when both this line and the one before it in the join have payload.
 */
static bool has_join_space(const struct BodyLineList* lines, ptrdiff_t start, ptrdiff_t index)
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
static ptrdiff_t filled_line_end(const struct FillParagraph* paragraph,
                                 ptrdiff_t atom_index,
                                 ptrdiff_t line_prefix_columns,
                                 ptrdiff_t width) __attribute__((nonnull(1)));

/**
 * @brief Returns the number of lines `append_filled_paragraph` builds from `paragraph`.
 *
 * @param paragraph      Split paragraph. Must not be `NULL`.
 * @param prefix_columns Continuation prefix columns.
 * @param width          Wrapping column.
 * @return Filled line count, at least 1.
 */
static ptrdiff_t filled_line_count(const struct FillParagraph* paragraph,
                                   ptrdiff_t prefix_columns,
                                   ptrdiff_t width) __attribute__((nonnull(1)));

/**
 * @brief Appends greedily filled lines from one split paragraph.
 *
 * @param lines          Line list that receives the filled paragraph. Its array has room for every
 *                       line the paragraph fills. Must not be `NULL`.
 * @param paragraph      Split paragraph. Must not be `NULL`.
 * @param prefix_columns Continuation prefix columns.
 * @param width          Wrapping column.
 * @param arena          Arena that owns the new line texts. Must not be `NULL`.
 */
static void append_filled_paragraph(struct BodyLineList* lines,
                                    const struct FillParagraph* paragraph,
                                    ptrdiff_t prefix_columns,
                                    ptrdiff_t width,
                                    struct Arena* arena) __attribute__((nonnull(1, 2, 5)));

/**
 * @brief Appends one filled line: a lead followed by `atoms` joined with single spaces.
 *
 * @param lines        Line list that receives the line. Its array has room for it. Must not be
 *                     `NULL`.
 * @param kind         Kind assigned to the line.
 * @param lead         Bytes that open the line, such as a frozen tag prefix.
 * @param lead_columns Spaces that open the line after `lead`, where a wrapped description
 *                     continues.
 * @param atoms        Atoms placed after the lead. May be `NULL` only when `atom_count` is 0.
 * @param atom_count   Number of atoms in `atoms`.
 * @param arena        Arena that owns the line text. Must not be `NULL`.
 */
static void append_filled_line(struct BodyLineList* lines,
                               enum BodyLineKind kind,
                               struct Str lead,
                               ptrdiff_t lead_columns,
                               const struct Atom* atoms,
                               ptrdiff_t atom_count,
                               struct Arena* arena) __attribute__((nonnull(1, 7)));

/**
 * @brief Merges each atom that would open a line of another kind into the atom before it.
 *
 * A continuation line that started with a Doxygen command, a list marker, or a tool directive would
 * read back as a new tag, list item, or directive and stop the paragraph from refilling, so such an
 * atom travels with the word before it. An atom inside the frozen tag prefix takes nothing, because
 * the atom after the prefix always stays on the first line.
 *
 * @param atoms             Atoms of `joined`, merged in place. Must not be `NULL`.
 * @param joined            Paragraph text the atoms are views into.
 * @param frozen_prefix_len Bytes at the start of `joined` that stay on the first line.
 */
static void glue_line_opener_atoms(struct AtomList* atoms,
                                   struct Str joined,
                                   ptrdiff_t frozen_prefix_len) __attribute__((nonnull(1)));

/**
 * @brief Reports whether adding columns would exceed the wrapping width.
 *
 * @param prefix_columns Columns occupied before the payload.
 * @param used_columns   Payload columns already on the line.
 * @param extra_columns  Columns proposed for the line.
 * @param width          Wrapping column.
 * @return `true` when the combined columns exceed `width`, or `false` otherwise.
 */
static bool is_width_exceeded(ptrdiff_t prefix_columns,
                              ptrdiff_t used_columns,
                              ptrdiff_t extra_columns,
                              ptrdiff_t width);

/**
 * @brief Returns the length of a frozen Doxygen tag prefix at the start of `text`.
 *
 * @param text Payload.
 * @return Byte length of the tag keyword plus any documented name and the following spaces, or 0.
 */
static ptrdiff_t tag_prefix_len(struct Str text);

/**
 * @brief Returns the offset in `joined` just past the last byte of `atom`.
 *
 * @param atom   Atom that is a view into `joined`. Must not be `NULL`.
 * @param joined Paragraph text.
 * @return Offset immediately after the atom.
 */
static ptrdiff_t atom_end_offset(const struct Atom* atom, struct Str joined)
    __attribute__((nonnull(1)));

void fill_body_lines(struct BodyLineList* lines,
                     ptrdiff_t prefix_columns_first,
                     ptrdiff_t prefix_columns,
                     ptrdiff_t width,
                     bool has_trailing_closer,
                     struct Arena* arena) {
  // Each paragraph holds at least one line, so the line count bounds the paragraph count.
  struct FillParagraph* paragraphs = arena_new(arena, lines->count, struct FillParagraph);
  ptrdiff_t paragraph_count = 0;
  // A copied line stays one line.
  ptrdiff_t capacity = 0;
  ptrdiff_t i = 0;
  while (i < lines->count) {
    if (lines->items[i].kind != BODY_LINE_PROSE && lines->items[i].kind != BODY_LINE_TAG) {
      capacity++;
      i++;
      continue;
    }
    const ptrdiff_t start = i;
    i++;
    while (i < lines->count && lines->items[i].kind == BODY_LINE_PROSE) {
      i++;
    }
    paragraphs[paragraph_count] = split_paragraph(lines, start, i, arena);
    struct FillParagraph* paragraph = &paragraphs[paragraph_count];
    // Only the body's first line follows the opener. The closer trails the last body line, so only
    // a paragraph that ends the body makes room for it.
    paragraph->prefix_columns_first = start == 0 ? prefix_columns_first : prefix_columns;
    paragraph->closer_columns = has_trailing_closer && i == lines->count ? 3 : 0;
    capacity += filled_line_count(paragraph, prefix_columns, width);
    paragraph_count++;
  }

  struct BodyLineList rebuilt = {
      .items = arena_new(arena, capacity, struct BodyLine),
      .count = 0,
  };
  ptrdiff_t paragraph_index = 0;
  i = 0;
  while (i < lines->count) {
    if (paragraph_index == paragraph_count || paragraphs[paragraph_index].start != i) {
      rebuilt.items[rebuilt.count] = lines->items[i];
      rebuilt.count++;
      i++;
      continue;
    }
    const struct FillParagraph* paragraph = &paragraphs[paragraph_index];
    append_filled_paragraph(&rebuilt, paragraph, prefix_columns, width, arena);
    i = paragraph->end;
    paragraph_index++;
  }
  lines->items = rebuilt.items;
  lines->count = rebuilt.count;
}

static struct FillParagraph split_paragraph(const struct BodyLineList* lines,
                                            ptrdiff_t start,
                                            ptrdiff_t end,
                                            struct Arena* arena) {
  const struct Str joined = join_payloads(lines, start, end, arena);
  const ptrdiff_t frozen_prefix_len =
      lines->items[start].kind == BODY_LINE_TAG ? tag_prefix_len(joined) : 0;
  struct AtomList atoms = atom_split(joined, arena);
  glue_line_opener_atoms(&atoms, joined, frozen_prefix_len);
  // The frozen prefix opens the first line, so the atoms inside it (the tag and name) are dropped
  // rather than written twice.
  ptrdiff_t prefix_atom_count = 0;
  while (prefix_atom_count < atoms.count &&
         atom_end_offset(&atoms.items[prefix_atom_count], joined) <= frozen_prefix_len) {
    prefix_atom_count++;
  }
  return (struct FillParagraph){
      .start = start,
      .end = end,
      .joined = joined,
      .frozen_prefix_len = frozen_prefix_len,
      .tag_indent_columns = atom_column_width(str_head(joined, frozen_prefix_len)),
      .atoms = {.items = atoms.items + prefix_atom_count, .count = atoms.count - prefix_atom_count},
  };
}

static struct Str join_payloads(const struct BodyLineList* lines,
                                ptrdiff_t start,
                                ptrdiff_t end,
                                struct Arena* arena) {
  // Each piece extends the text before it in place, so each byte is copied once.
  struct Str joined = {0};
  for (ptrdiff_t i = start; i < end; i++) {
    if (has_join_space(lines, start, i)) {
      joined = str_concat(joined, S(" "), arena);
    }
    joined = str_concat(joined, lines->items[i].text, arena);
  }
  return joined;
}

static bool has_join_space(const struct BodyLineList* lines, ptrdiff_t start, ptrdiff_t index) {
  return index > start && lines->items[index - 1].text.len > 0 && lines->items[index].text.len > 0;
}

static ptrdiff_t filled_line_end(const struct FillParagraph* paragraph,
                                 ptrdiff_t atom_index,
                                 ptrdiff_t line_prefix_columns,
                                 ptrdiff_t width) {
  const struct AtomList* atoms = &paragraph->atoms;
  // The first atom on a line needs no gap: it follows the prefix, or sits at the description column
  // after a continuation indent.
  ptrdiff_t used_columns = paragraph->tag_indent_columns;
  ptrdiff_t line_end = atom_index;
  while (line_end < atoms->count) {
    const bool is_line_start = line_end == atom_index;
    const ptrdiff_t gap = is_line_start ? 0 : 1;
    const bool is_last = line_end + 1 == atoms->count;
    const ptrdiff_t extra_columns =
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

static ptrdiff_t filled_line_count(const struct FillParagraph* paragraph,
                                   ptrdiff_t prefix_columns,
                                   ptrdiff_t width) {
  ptrdiff_t line_count = 0;
  ptrdiff_t atom_index = 0;
  do {
    const ptrdiff_t line_prefix_columns =
        line_count == 0 ? paragraph->prefix_columns_first : prefix_columns;
    atom_index = filled_line_end(paragraph, atom_index, line_prefix_columns, width);
    line_count++;
  } while (atom_index < paragraph->atoms.count);
  return line_count;
}

static void append_filled_paragraph(struct BodyLineList* lines,
                                    const struct FillParagraph* paragraph,
                                    ptrdiff_t prefix_columns,
                                    ptrdiff_t width,
                                    struct Arena* arena) {
  // The frozen prefix opens the first line. Overflow lines indent to the same column so a wrapped
  // description stays under the first.
  ptrdiff_t atom_index = 0;
  bool is_first_line = true;
  do {
    const ptrdiff_t line_prefix_columns =
        is_first_line ? paragraph->prefix_columns_first : prefix_columns;
    const ptrdiff_t line_end = filled_line_end(paragraph, atom_index, line_prefix_columns, width);
    const bool is_tag_line = is_first_line && paragraph->frozen_prefix_len > 0;
    const struct Str lead =
        is_first_line ? str_head(paragraph->joined, paragraph->frozen_prefix_len) : S("");
    const ptrdiff_t lead_columns = is_first_line ? 0 : paragraph->tag_indent_columns;
    append_filled_line(lines, is_tag_line ? BODY_LINE_TAG : BODY_LINE_PROSE, lead, lead_columns,
                       paragraph->atoms.items + atom_index, line_end - atom_index, arena);
    atom_index = line_end;
    is_first_line = false;
  } while (atom_index < paragraph->atoms.count);
}

static void append_filled_line(struct BodyLineList* lines,
                               enum BodyLineKind kind,
                               struct Str lead,
                               ptrdiff_t lead_columns,
                               const struct Atom* atoms,
                               ptrdiff_t atom_count,
                               struct Arena* arena) {
  // The line is measured before it is built, so it takes one allocation of its exact length. A
  // buffer grown as it is written would double past that length, and a line hanging under a long
  // `@param` name is long enough for the doubling to double the scratch region its block needs.
  ptrdiff_t len = lead.len + lead_columns;
  for (ptrdiff_t i = 0; i < atom_count; i++) {
    len += (i > 0 ? 1 : 0) + atoms[i].text.len;
  }
  char* text = arena_new(arena, len, char);
  // `memcpy` requires valid pointers even for zero bytes, and an empty lead may hold `NULL`.
  if (lead.len > 0) {
    memcpy(text, lead.data, (size_t)lead.len);
  }
  ptrdiff_t offset = lead.len;
  memset(text + offset, ' ', (size_t)lead_columns);
  offset += lead_columns;
  for (ptrdiff_t i = 0; i < atom_count; i++) {
    if (i > 0) {
      text[offset] = ' ';
      offset++;
    }
    memcpy(text + offset, atoms[i].text.data, (size_t)atoms[i].text.len);
    offset += atoms[i].text.len;
  }
  lines->items[lines->count] = (struct BodyLine){.kind = kind, .text = {text, len}};
  lines->count++;
}

static void glue_line_opener_atoms(struct AtomList* atoms,
                                   struct Str joined,
                                   ptrdiff_t frozen_prefix_len) {
  ptrdiff_t kept = 0;
  for (ptrdiff_t i = 0; i < atoms->count; i++) {
    const struct Atom atom = atoms->items[i];
    struct DoxygenTag tag;
    // Each atom is followed in `joined` by the space or end that follows it on an emitted line, so
    // the line classifiers read on from the atom to the end of `joined`.
    const struct Str rest = str_tail(joined, atom.text.data - joined.data);
    const bool is_line_opener = doxygen_has_tag(atom.text, &tag) || comment_is_list_line(rest) ||
                                comment_is_tool_directive(rest);
    if (kept > 0 && is_line_opener) {
      struct Atom* previous = &atoms->items[kept - 1];
      if (atom_end_offset(previous, joined) > frozen_prefix_len) {
        previous->text.len = atom.text.data + atom.text.len - previous->text.data;
        previous->columns = atom_column_width(previous->text);
        continue;
      }
    }
    atoms->items[kept] = atom;
    kept++;
  }
  atoms->count = kept;
}

static bool is_width_exceeded(ptrdiff_t prefix_columns,
                              ptrdiff_t used_columns,
                              ptrdiff_t extra_columns,
                              ptrdiff_t width) {
  return prefix_columns + used_columns + extra_columns > width;
}

static ptrdiff_t tag_prefix_len(struct Str text) {
  struct DoxygenTag tag;
  return doxygen_has_tag(text, &tag) ? tag.description_offset : 0;
}

static ptrdiff_t atom_end_offset(const struct Atom* atom, struct Str joined) {
  return atom->text.data + atom->text.len - joined.data;
}
