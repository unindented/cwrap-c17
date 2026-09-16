#ifndef CWRAP_ATOM_H
#define CWRAP_ATOM_H

#include <stddef.h>

struct Arena;

/** One unbreakable wrap unit and its display width. */
struct Atom {
  /** Payload bytes. Not terminated; `text_len` is the length. */
  const char* text;

  /** Number of bytes in `text`. */
  size_t text_len;

  /** Display columns occupied by `text`. */
  size_t columns;
};

/** Arena-owned list of atoms. */
struct AtomList {
  /** Atom array, or `NULL` when `count` is 0. */
  struct Atom* items;

  /** Number of atoms in `items`. */
  size_t count;
};

/**
 * @brief Returns the display-column width of `text`.
 *
 * A code point whose Unicode 16.0 East Asian Width property is `W` or `F` counts as 2. Combining
 * marks, format characters, and C0 controls other than tab count as 0. Invalid UTF-8 bytes count as
 * 1. Tab is not expected inside comment payloads.
 *
 * @param text     Bytes to measure. Must hold at least `text_len` bytes. May be `NULL` only when
 *                 `text_len` is 0.
 * @param text_len Number of bytes in `text`.
 * @return Display columns occupied by `text`.
 */
size_t atom_column_width(const char* text, size_t text_len);

/**
 * @brief Splits `text` into unbreakable wrap atoms.
 *
 * A backtick span, an uppercase hex-byte run (`E2 80 94`), and a quantity-plus-unit (`264 MB`) stay
 * together. A parenthetical wraps at its spaces, the same as ordinary words. A trailing `.` or `,`,
 * a possessive `'s`, and a hyphenated suffix travel with the atom they follow.
 *
 * @param text      Paragraph payload. Must hold at least `text_len` bytes. May be `NULL` only when
 *                  `text_len` is 0.
 * @param text_len  Number of bytes in `text`.
 * @param arena     Arena that owns the atom array. Atom `text` pointers alias `text`. Must not be
 *                  `NULL`.
 * @param atoms_out Receives the atom list. Written only on success. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
int atom_split(const char* text, size_t text_len, struct Arena* arena, struct AtomList* atoms_out)
    __attribute__((nonnull(3, 4)));

#endif
