#include "domain/atom.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ascii.h"
#include "shared/arena.h"
#include "shared/str.h"

#include "domain/unicode_width_data.h"

/**
 * @brief Decodes one UTF-8 code point at `text.data[index]`.
 *
 * @param text          Bytes to decode from.
 * @param index         Offset to decode from. Must be less than `text.len`.
 * @param codepoint_out Receives the code point. An invalid sequence yields the first byte as a
 *                      value. Must not be `NULL`.
 * @return Number of bytes consumed, at least 1.
 */
static ptrdiff_t utf8_next(struct Str text, ptrdiff_t index, uint32_t* codepoint_out)
    __attribute__((nonnull(3)));

/**
 * @brief Returns the display-column width of one Unicode code point.
 *
 * @param codepoint Code point to measure.
 * @return 2 for East Asian wide or fullwidth, 0 for combining marks, format characters, and C0
 *         controls, else 1.
 */
static ptrdiff_t codepoint_columns(uint32_t codepoint);

/**
 * @brief Reports whether `codepoint` is inside one of `ranges`.
 *
 * @param codepoint Code point to find.
 * @param ranges    Sorted inclusive ranges. Must not be `NULL`.
 * @param count     Number of ranges.
 * @return `true` when a range contains `codepoint`.
 */
static bool codepoint_in_ranges(uint32_t codepoint,
                                const struct CodepointRange* ranges,
                                ptrdiff_t count) __attribute__((nonnull(2)));

/**
 * @brief Reports whether `c` is an ASCII hex digit.
 *
 * @param c Byte to classify.
 * @return `true` when `c` is in `[0-9A-F]`. Lowercase letters are English, not hex.
 */
static bool is_hex(unsigned char c);

/**
 * @brief Returns the length of a backtick span starting at `index`, or 0 if none.
 *
 * @param text  Payload bytes.
 * @param index Offset of a candidate opening backtick.
 * @return Byte length of the span including both backticks, or 0 when `index` holds no backtick or
 *         no backtick closes the span there.
 */
static ptrdiff_t backtick_span_len(struct Str text, ptrdiff_t index);

/**
 * @brief Returns the length of a hex-byte run starting at `index`, or 0 if none.
 *
 * @param text  Payload bytes.
 * @param index Start offset.
 * @return Byte length of two or more `HH` groups separated by spaces, or 0. A following letter
 *         rejects the match so `be able` is not a hex run.
 */
static ptrdiff_t hex_run_len(struct Str text, ptrdiff_t index);

/**
 * @brief Returns the length of a quantity-plus-unit starting at `index`, or 0 if none.
 *
 * @param text  Payload bytes.
 * @param index Start offset.
 * @return Byte length of `digits` plus a known unit such as `MB`, or 0.
 */
static ptrdiff_t quantity_unit_len(struct Str text, ptrdiff_t index);

/**
 * @brief Reports whether `text` is a known quantity unit such as `MB`.
 *
 * @param text Unit bytes.
 * @return `true` when `text` is an allowed unit.
 */
static bool is_quantity_unit(struct Str text);

/**
 * @brief Extends `head_len` through any immediately following non-whitespace.
 *
 * A possessive `'s`, a trailing `.` or `,`, and a hyphenated suffix such as `-aligned` stay on the
 * atom. Fill joins atoms with a space, so leaving glued text for a later word would invent a break.
 * A backtick span that starts the atom, or follows only opening brackets at its start, is taken
 * whole, spaces included, so its closer does not open a span of its own. A backtick quoted between
 * brackets opens nothing.
 *
 * @param text     Payload bytes.
 * @param index    Start of the atom.
 * @param head_len Length of the atom's start, such as a hex run, or 0.
 * @return Extended length.
 */
static ptrdiff_t attach_trailer(struct Str text, ptrdiff_t index, ptrdiff_t head_len);

/**
 * @brief Reports whether a backtick span may open after `c` inside a word.
 *
 * @param c Byte before the backtick.
 * @return `true` for an opening bracket: one of `([{`.
 */
static bool is_span_lead(unsigned char c);

/**
 * @brief Reports whether `c` closes a bracket, so a backtick between it and a lead is quoted.
 *
 * @param c Byte after the backtick.
 * @return `true` for a closing bracket: one of `)]}`.
 */
static bool is_span_close(unsigned char c);

ptrdiff_t atom_column_width(struct Str text) {
  ptrdiff_t columns = 0;
  ptrdiff_t i = 0;
  while (i < text.len) {
    uint32_t codepoint = 0;
    const ptrdiff_t consumed = utf8_next(text, i, &codepoint);
    columns += codepoint_columns(codepoint);
    i += consumed;
  }
  return columns;
}

struct AtomList atom_split(struct Str text, struct Arena* arena) {
  ptrdiff_t count = 0;
  ptrdiff_t i = 0;
  while (i < text.len) {
    if (text.data[i] == ' ' || text.data[i] == '\t') {
      i++;
      continue;
    }
    count++;
    const ptrdiff_t len = atom_len(text, i);
    i += len;
  }

  struct Atom* items = NULL;
  if (count > 0) {
    items = arena_new(arena, count, struct Atom);
    ptrdiff_t n = 0;
    i = 0;
    while (i < text.len) {
      if (text.data[i] == ' ' || text.data[i] == '\t') {
        i++;
        continue;
      }
      const ptrdiff_t len = atom_len(text, i);
      items[n].text = (struct Str){text.data + i, len};
      items[n].columns = atom_column_width(items[n].text);
      n++;
      i += len;
    }
  }

  return (struct AtomList){.items = items, .count = count};
}

ptrdiff_t atom_len(struct Str text, ptrdiff_t index) {
  ptrdiff_t len = hex_run_len(text, index);
  if (len == 0) {
    len = quantity_unit_len(text, index);
  }
  return attach_trailer(text, index, len);
}

static ptrdiff_t utf8_next(struct Str text, ptrdiff_t index, uint32_t* codepoint_out) {
  const unsigned char lead = (unsigned char)text.data[index];
  if (lead < 0x80) {
    *codepoint_out = lead;
    return 1;
  }
  ptrdiff_t sequence_len = 0;
  uint32_t codepoint = 0;
  if ((lead & 0xE0) == 0xC0) {
    sequence_len = 2;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    sequence_len = 3;
    codepoint = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    sequence_len = 4;
    codepoint = lead & 0x07;
  } else {
    *codepoint_out = lead;
    return 1;
  }
  if (index + sequence_len > text.len) {
    *codepoint_out = lead;
    return 1;
  }
  for (ptrdiff_t n = 1; n < sequence_len; n++) {
    const unsigned char continuation = (unsigned char)text.data[index + n];
    if ((continuation & 0xC0) != 0x80) {
      *codepoint_out = lead;
      return 1;
    }
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  const bool is_overlong = (sequence_len == 2 && codepoint < 0x80) ||
                           (sequence_len == 3 && codepoint < 0x800) ||
                           (sequence_len == 4 && codepoint < 0x10000);
  if (is_overlong || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
    *codepoint_out = lead;
    return 1;
  }
  *codepoint_out = codepoint;
  return sequence_len;
}

static ptrdiff_t codepoint_columns(uint32_t codepoint) {
  if (codepoint < 0x20 || codepoint == 0x7F) {
    return 0;
  }
  if (codepoint_in_ranges(
          codepoint, unicode_zero_width_ranges,
          sizeof(unicode_zero_width_ranges) / sizeof(unicode_zero_width_ranges[0]))) {
    return 0;
  }
  if (codepoint_in_ranges(codepoint, unicode_wide_ranges,
                          sizeof(unicode_wide_ranges) / sizeof(unicode_wide_ranges[0]))) {
    return 2;
  }
  return 1;
}

static bool codepoint_in_ranges(uint32_t codepoint,
                                const struct CodepointRange* ranges,
                                ptrdiff_t count) {
  ptrdiff_t low = 0;
  ptrdiff_t high = count;
  while (low < high) {
    const ptrdiff_t middle = low + (high - low) / 2;
    const struct CodepointRange* range = &ranges[middle];
    if (codepoint < range->first) {
      high = middle;
    } else if (codepoint > range->last) {
      low = middle + 1;
    } else {
      return true;
    }
  }
  return false;
}

static bool is_hex(unsigned char c) {
  return ascii_is_digit(c) || (c >= 'A' && c <= 'F');
}

static ptrdiff_t backtick_span_len(struct Str text, ptrdiff_t index) {
  if (index >= text.len || text.data[index] != '`') {
    return 0;
  }
  for (ptrdiff_t i = index + 1; i < text.len; i++) {
    if (text.data[i] == '`') {
      return i + 1 - index;
    }
  }
  return 0;
}

static ptrdiff_t hex_run_len(struct Str text, ptrdiff_t index) {
  if (index + 2 > text.len) {
    return 0;
  }
  if (!is_hex((unsigned char)text.data[index]) || !is_hex((unsigned char)text.data[index + 1])) {
    return 0;
  }
  ptrdiff_t i = index + 2;
  ptrdiff_t groups = 1;
  while (i + 3 <= text.len && text.data[i] == ' ' && is_hex((unsigned char)text.data[i + 1]) &&
         is_hex((unsigned char)text.data[i + 2])) {
    i += 3;
    groups++;
  }
  if (groups < 2) {
    return 0;
  }
  // `be able` looks like `be` plus `ab` until the leftover `le` is noticed. A real byte run ends at
  // whitespace or punctuation.
  if (i < text.len && ascii_is_alphanumeric((unsigned char)text.data[i])) {
    return 0;
  }
  return i - index;
}

static ptrdiff_t quantity_unit_len(struct Str text, ptrdiff_t index) {
  if (index >= text.len || !ascii_is_digit((unsigned char)text.data[index])) {
    return 0;
  }
  ptrdiff_t i = index;
  while (i < text.len && ascii_is_digit((unsigned char)text.data[i])) {
    i++;
  }
  if (i < text.len && text.data[i] == '.') {
    const ptrdiff_t after_dot = i + 1;
    if (after_dot < text.len && ascii_is_digit((unsigned char)text.data[after_dot])) {
      i = after_dot;
      while (i < text.len && ascii_is_digit((unsigned char)text.data[i])) {
        i++;
      }
    }
  }
  if (i >= text.len || text.data[i] != ' ') {
    return 0;
  }
  i++;
  const ptrdiff_t unit_start = i;
  while (i < text.len && ((text.data[i] >= 'A' && text.data[i] <= 'Z') ||
                          (text.data[i] >= 'a' && text.data[i] <= 'z'))) {
    i++;
  }
  const ptrdiff_t unit_len = i - unit_start;
  if (!is_quantity_unit((struct Str){text.data + unit_start, unit_len})) {
    return 0;
  }
  return i - index;
}

static bool is_quantity_unit(struct Str text) {
  static const struct Str units[] = {
      STR_INIT("MB"),  STR_INIT("KB"),  STR_INIT("GB"),  STR_INIT("TB"),  STR_INIT("PB"),
      STR_INIT("KiB"), STR_INIT("MiB"), STR_INIT("GiB"), STR_INIT("TiB"), STR_INIT("kB"),
      STR_INIT("ms"),  STR_INIT("us"),  STR_INIT("ns"),  STR_INIT("ps"),  STR_INIT("Hz"),
      STR_INIT("kHz"), STR_INIT("MHz"), STR_INIT("GHz"), STR_INIT("px"),  STR_INIT("em"),
      STR_INIT("pt")};
  for (ptrdiff_t u = 0; u < (ptrdiff_t)(sizeof(units) / sizeof(units[0])); u++) {
    if (str_eq(text, units[u])) {
      return true;
    }
  }
  return false;
}

static ptrdiff_t attach_trailer(struct Str text, ptrdiff_t index, ptrdiff_t head_len) {
  ptrdiff_t end = index + head_len;
  bool is_in_leads = head_len == 0;
  while (end < text.len && text.data[end] != ' ' && text.data[end] != '\t') {
    const bool is_quoted =
        end > index && end + 1 < text.len && is_span_close((unsigned char)text.data[end + 1]);
    const ptrdiff_t span_len = is_in_leads && !is_quoted ? backtick_span_len(text, end) : 0;
    if (span_len > 0) {
      end += span_len;
      is_in_leads = false;
      continue;
    }
    is_in_leads = is_in_leads && is_span_lead((unsigned char)text.data[end]);
    end++;
  }
  return end - index;
}

static bool is_span_lead(unsigned char c) {
  return c == '(' || c == '[' || c == '{';
}

static bool is_span_close(unsigned char c) {
  return c == ')' || c == ']' || c == '}';
}
