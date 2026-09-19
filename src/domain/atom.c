#include "domain/atom.h"

#include <stdbool.h>
#include <string.h>

#include "core/ascii.h"
#include "shared/arena.h"

#include "domain/unicode_width_data.h"

/**
 * @brief Decodes one UTF-8 code point at `text[index]`.
 *
 * @param text          Byte buffer. Must not be `NULL`.
 * @param text_len      Number of bytes in `text`.
 * @param index         Offset to decode from. Must be less than `text_len`.
 * @param codepoint_out Receives the code point. An invalid sequence yields the first byte as a
 *                      value. Must not be `NULL`.
 * @return Number of bytes consumed, at least 1.
 */
static size_t utf8_next(const char* text, size_t text_len, size_t index, uint32_t* codepoint_out)
    __attribute__((nonnull(1, 4)));

/**
 * @brief Returns the display-column width of one Unicode code point.
 *
 * @param codepoint Code point to measure.
 * @return 2 for East Asian wide or fullwidth, 0 for combining marks, format characters, and C0
 *         controls, else 1.
 */
static size_t codepoint_columns(uint32_t codepoint);

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
                                size_t count) __attribute__((nonnull(2)));

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
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param index    Offset of a candidate opening backtick.
 * @return Byte length of the span including both backticks, or 0.
 */
static size_t backtick_span_len(const char* text, size_t text_len, size_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the length of a hex-byte run starting at `index`, or 0 if none.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param index    Start offset.
 * @return Byte length of two or more `HH` groups separated by spaces, or 0. A following letter
 *         rejects the match so `be able` is not a hex run.
 */
static size_t hex_run_len(const char* text, size_t text_len, size_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the length of a quantity-plus-unit starting at `index`, or 0 if none.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param index    Start offset.
 * @return Byte length of `digits` plus a known unit such as `MB`, or 0.
 */
static size_t quantity_unit_len(const char* text, size_t text_len, size_t index)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `text` is a known quantity unit such as `MB`.
 *
 * @param text     Unit bytes. Must not be `NULL`.
 * @param text_len Number of bytes in `text`.
 * @return `true` when `text` is an allowed unit.
 */
static bool is_quantity_unit(const char* text, size_t text_len) __attribute__((nonnull(1)));

/**
 * @brief Returns the length of a whitespace-delimited word starting at `index`.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param index    Start offset. Must be less than `text_len`.
 * @return Byte length of the word.
 */
static size_t word_len(const char* text, size_t text_len, size_t index) __attribute__((nonnull(1)));

/**
 * @brief Extends `atom_len` through any immediately following non-whitespace.
 *
 * A possessive `'s`, a trailing `.` or `,`, and a hyphenated suffix such as `-aligned` stay on the
 * atom. Fill joins atoms with a space, so leaving glued text for a later word would invent a break.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Length of `text`.
 * @param index    Start of the atom.
 * @param atom_len Current atom length.
 * @return Extended length.
 */
static size_t attach_trailer(const char* text, size_t text_len, size_t index, size_t atom_len)
    __attribute__((nonnull(1)));

/**
 * @brief Returns the complete atom length at `index`.
 *
 * @param text     Payload bytes. Must not be `NULL`.
 * @param text_len Number of bytes in `text`.
 * @param index    Start of the atom. Must be less than `text_len`.
 * @return Atom length in bytes.
 */
static size_t atom_length_at(const char* text, size_t text_len, size_t index)
    __attribute__((nonnull(1)));

size_t atom_column_width(const char* text, size_t text_len) {
  if (text_len == 0) {
    return 0;
  }
  size_t columns = 0;
  size_t i = 0;
  while (i < text_len) {
    uint32_t codepoint = 0;
    const size_t consumed = utf8_next(text, text_len, i, &codepoint);
    columns += codepoint_columns(codepoint);
    i += consumed;
  }
  return columns;
}

int atom_split(const char* text, size_t text_len, struct Arena* arena, struct AtomList* atoms_out) {
  size_t count = 0;
  size_t i = 0;
  while (i < text_len) {
    if (text[i] == ' ' || text[i] == '\t') {
      i++;
      continue;
    }
    count++;
    const size_t len = atom_length_at(text, text_len, i);
    i += len;
  }

  struct Atom* items = NULL;
  if (count > 0) {
    items = arena_calloc(arena, count, sizeof(*items));
    if (items == NULL) {
      return -1;
    }
    size_t n = 0;
    i = 0;
    while (i < text_len) {
      if (text[i] == ' ' || text[i] == '\t') {
        i++;
        continue;
      }
      const size_t len = atom_length_at(text, text_len, i);
      items[n].text = text + i;
      items[n].text_len = len;
      items[n].columns = atom_column_width(text + i, len);
      n++;
      i += len;
    }
  }

  atoms_out->items = items;
  atoms_out->count = count;
  return 0;
}

static size_t utf8_next(const char* text, size_t text_len, size_t index, uint32_t* codepoint_out) {
  const unsigned char lead = (unsigned char)text[index];
  if (lead < 0x80) {
    *codepoint_out = lead;
    return 1;
  }
  size_t bytes_needed = 0;
  uint32_t codepoint = 0;
  if ((lead & 0xE0) == 0xC0) {
    bytes_needed = 2;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    bytes_needed = 3;
    codepoint = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    bytes_needed = 4;
    codepoint = lead & 0x07;
  } else {
    *codepoint_out = lead;
    return 1;
  }
  if (index + bytes_needed > text_len) {
    *codepoint_out = lead;
    return 1;
  }
  for (size_t n = 1; n < bytes_needed; n++) {
    const unsigned char continuation = (unsigned char)text[index + n];
    if ((continuation & 0xC0) != 0x80) {
      *codepoint_out = lead;
      return 1;
    }
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  const bool is_overlong = (bytes_needed == 2 && codepoint < 0x80) ||
                           (bytes_needed == 3 && codepoint < 0x800) ||
                           (bytes_needed == 4 && codepoint < 0x10000);
  if (is_overlong || (codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
    *codepoint_out = lead;
    return 1;
  }
  *codepoint_out = codepoint;
  return bytes_needed;
}

static size_t codepoint_columns(uint32_t codepoint) {
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
                                size_t count) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
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

static size_t backtick_span_len(const char* text, size_t text_len, size_t index) {
  if (index >= text_len || text[index] != '`') {
    return 0;
  }
  for (size_t i = index + 1; i < text_len; i++) {
    if (text[i] == '`') {
      return i + 1 - index;
    }
  }
  return text_len - index;
}

static size_t hex_run_len(const char* text, size_t text_len, size_t index) {
  if (index + 2 > text_len) {
    return 0;
  }
  if (!is_hex((unsigned char)text[index]) || !is_hex((unsigned char)text[index + 1])) {
    return 0;
  }
  size_t i = index + 2;
  size_t groups = 1;
  while (i + 3 <= text_len && text[i] == ' ' && is_hex((unsigned char)text[i + 1]) &&
         is_hex((unsigned char)text[i + 2])) {
    i += 3;
    groups++;
  }
  if (groups < 2) {
    return 0;
  }
  // `be able` looks like `be` plus `ab` until the leftover `le` is noticed. A real byte run ends at
  // whitespace or punctuation.
  if (i < text_len && ascii_is_alphanumeric((unsigned char)text[i])) {
    return 0;
  }
  return i - index;
}

static size_t quantity_unit_len(const char* text, size_t text_len, size_t index) {
  if (index >= text_len || !ascii_is_digit((unsigned char)text[index])) {
    return 0;
  }
  size_t i = index;
  while (i < text_len && ascii_is_digit((unsigned char)text[i])) {
    i++;
  }
  if (i < text_len && text[i] == '.') {
    const size_t after_dot = i + 1;
    if (after_dot < text_len && ascii_is_digit((unsigned char)text[after_dot])) {
      i = after_dot;
      while (i < text_len && ascii_is_digit((unsigned char)text[i])) {
        i++;
      }
    }
  }
  if (i >= text_len || text[i] != ' ') {
    return 0;
  }
  i++;
  const size_t unit_start = i;
  while (i < text_len &&
         ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) {
    i++;
  }
  const size_t unit_len = i - unit_start;
  if (!is_quantity_unit(text + unit_start, unit_len)) {
    return 0;
  }
  return i - index;
}

static bool is_quantity_unit(const char* text, size_t text_len) {
  static const char* const units[] = {"MB",  "KB",  "GB",  "TB",  "PB", "KiB", "MiB",
                                      "GiB", "TiB", "kB",  "ms",  "us", "ns",  "ps",
                                      "Hz",  "kHz", "MHz", "GHz", "px", "em",  "pt"};
  for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
    if (strlen(units[u]) == text_len && memcmp(text, units[u], text_len) == 0) {
      return true;
    }
  }
  return false;
}

static size_t word_len(const char* text, size_t text_len, size_t index) {
  size_t i = index;
  while (i < text_len && text[i] != ' ' && text[i] != '\t') {
    i++;
  }
  return i - index;
}

static size_t attach_trailer(const char* text, size_t text_len, size_t index, size_t atom_len) {
  size_t end = index + atom_len;
  while (end < text_len && text[end] != ' ' && text[end] != '\t') {
    end++;
  }
  return end - index;
}

static size_t atom_length_at(const char* text, size_t text_len, size_t index) {
  size_t len = backtick_span_len(text, text_len, index);
  if (len == 0) {
    len = hex_run_len(text, text_len, index);
  }
  if (len == 0) {
    len = quantity_unit_len(text, text_len, index);
  }
  if (len == 0) {
    len = word_len(text, text_len, index);
  }
  return attach_trailer(text, text_len, index, len);
}
