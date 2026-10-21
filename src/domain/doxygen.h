#ifndef CWRAP_DOXYGEN_H
#define CWRAP_DOXYGEN_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Arena;
struct BodyLineList;

/** Parsed slices and offsets for one Doxygen command at the start of a payload. */
struct DoxygenTag {
  /** Command word after the `@` or `\` marker, a view into the parsed text. */
  struct Str command;

  /** Bytes from the start through the command word and any bracketed or braced option. */
  ptrdiff_t keyword_len;

  /** Name that a `param` or `retval` command documents, a view into the parsed text, or empty. */
  struct Str name;

  /** Offset where the description begins, after separating whitespace. */
  ptrdiff_t description_offset;
};

/**
 * @brief Parses a Doxygen command at the start of `text`.
 *
 * A command is `@` or `\` followed by ASCII letters and an optional `[...]` or `{...}` option with
 * no whitespace inside, such as `@param[in]` or `\code{.c}`. Whitespace or the end of `text` must
 * follow it. A `param` or `retval` command also takes the next word as the name it documents.
 *
 * @param text    Payload bytes.
 * @param tag_out Receives the parsed command when one is present. Must not be `NULL`.
 * @return `true` when `text` starts with a command, or `false` otherwise.
 */
bool doxygen_has_tag(struct Str text, struct DoxygenTag* tag_out) __attribute__((nonnull(2)));

/**
 * @brief Aligns the descriptions of `param` and `retval` tags to the widest name plus one space.
 *
 * Each of the two commands aligns separately, across the whole body. Other tags keep their own
 * description columns. The function rewrites tag line texts in place using `arena`.
 *
 * @param lines Body lines to align. Must not be `NULL`.
 * @param arena Arena that owns replacement line texts. Must not be `NULL`.
 */
void doxygen_align_parameters(struct BodyLineList* lines, struct Arena* arena)
    __attribute__((nonnull(1, 2)));

#endif
