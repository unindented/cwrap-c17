#ifndef CWRAP_DOXYGEN_H
#define CWRAP_DOXYGEN_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct BodyLineList;

/** Parsed slices and offsets for one Doxygen command at the start of a payload. */
struct DoxygenTag {
  /** Command word after the `@` or `\` marker, borrowed from the parsed text. */
  const char* command;

  /** Number of bytes in `command`. */
  size_t command_len;

  /** Bytes from the start through the command word and any bracketed or braced option. */
  size_t keyword_len;

  /** Name that a `param` or `retval` command documents, borrowed from the parsed text, or
     `NULL`. */
  const char* name;

  /** Number of bytes in `name`. */
  size_t name_len;

  /** Offset where the description begins, after separating whitespace. */
  size_t description_offset;
};

/**
 * @brief Parses a Doxygen command at the start of `text`.
 *
 * A command is `@` or `\` followed by ASCII letters and an optional `[...]` or `{...}` option with
 * no whitespace inside, such as `@param[in]` or `\code{.c}`. Whitespace or the end of `text` must
 * follow it. A `param` or `retval` command also takes the next word as the name it documents.
 *
 * @param text     Payload bytes. Must hold at least `text_len` bytes. May be `NULL` only when
 *                 `text_len` is 0.
 * @param text_len Number of bytes in `text`.
 * @param tag_out  Receives the parsed command when one is present. Must not be `NULL`.
 * @return `true` when `text` starts with a command, or `false` otherwise.
 */
bool doxygen_has_tag(const char* text, size_t text_len, struct DoxygenTag* tag_out)
    __attribute__((nonnull(3)));

/**
 * @brief Aligns the descriptions of `param` and `retval` tags to the widest name plus one space.
 *
 * Each of the two commands aligns separately, across the whole body. Other tags keep their own
 * description columns. The function rewrites tag line texts in place using `arena`.
 *
 * @param lines   Body lines to align. Must not be `NULL`.
 * @param arena   Arena that owns replacement line texts. Must not be `NULL`.
 * @param err     Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int doxygen_align_parameters(struct BodyLineList* lines,
                             struct Arena* arena,
                             char* err,
                             size_t err_len) __attribute__((nonnull(1, 2)));

#endif
