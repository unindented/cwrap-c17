#ifndef CWRAP_ATOM_H
#define CWRAP_ATOM_H

#include <stddef.h>

#include "shared/str.h"

struct Arena;

/** One unbreakable wrap unit and its display width. */
struct Atom {
  /** Payload bytes. */
  struct Str text;

  /** Display columns occupied by `text`. */
  ptrdiff_t columns;
};

/** Arena-owned list of atoms. */
struct AtomList {
  /** Atom array, or `NULL` when `count` is 0. */
  struct Atom* items;

  /** Number of atoms in `items`. */
  ptrdiff_t count;
};

/**
 * @brief Returns the display-column width of `text`.
 *
 * A code point whose Unicode 16.0 East Asian Width property is `W` or `F` counts as 2. Combining
 * marks, format characters, and C0 controls other than tab count as 0. Invalid UTF-8 bytes count as
 * 1. Tab is not expected inside comment payloads.
 *
 * @param text Bytes to measure.
 * @return Display columns occupied by `text`.
 */
ptrdiff_t atom_column_width(struct Str text);

/**
 * @brief Splits `text` into unbreakable wrap atoms.
 *
 * A backtick span, an uppercase hex-byte run (`E2 80 94`), and a quantity-plus-unit (`264 MB`) stay
 * together. An unclosed backtick opens an ordinary word. A parenthetical wraps at its spaces, the
 * same as ordinary words. A trailing `.` or `,`, a possessive `'s`, and a hyphenated suffix travel
 * with the atom they follow.
 *
 * @param text  Paragraph payload.
 * @param arena Arena that owns the atom array. Each atom's `text` is a view into `text`. Must not
 *              be `NULL`.
 * @return The atom list.
 */
struct AtomList atom_split(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Returns the length of the atom that starts at `index`, as `atom_split` splits it.
 *
 * @param text  Payload bytes.
 * @param index Start of the atom. Must be less than `text.len`.
 * @return Atom length in bytes.
 */
ptrdiff_t atom_len(struct Str text, ptrdiff_t index);

#endif
