#ifndef CWRAP_DOXYGEN_H
#define CWRAP_DOXYGEN_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct BodyLineList;

/** Supported Doxygen tag at the start of a comment payload. */
enum DoxygenTagKind {
  /** The payload does not start with a supported tag. */
  DOXYGEN_TAG_NONE,

  /** A `@brief` tag. */
  DOXYGEN_TAG_BRIEF,

  /** A `@param` tag, optionally with a direction qualifier. */
  DOXYGEN_TAG_PARAM,

  /** A `@return` tag. */
  DOXYGEN_TAG_RETURN,
};

/** Parsed slices and offsets for one supported Doxygen tag. */
struct DoxygenTag {
  /** Tag kind. */
  enum DoxygenTagKind kind;

  /** Bytes from the start through the tag and any direction qualifier. */
  size_t keyword_len;

  /** Parameter name borrowed from the parsed text, or `NULL` when no parameter name is present. */
  const char* parameter_name;

  /** Number of bytes in `parameter_name`. */
  size_t parameter_name_len;

  /** Offset where the description begins, after separating whitespace. */
  size_t description_offset;
};

/**
 * @brief Parses a supported Doxygen tag at the start of `text`.
 *
 * Recognizes exact `@brief`, `@return`, and `@param` tags. A parameter tag may carry `[in]`,
 * `[out]`, or `[in,out]` before its name.
 *
 * @param text    Terminated payload. Must not be `NULL`.
 * @param tag_out Receives the parsed tag when one is present. Must not be `NULL`.
 * @return `true` when a supported tag is present, or `false` otherwise.
 */
bool doxygen_has_tag(const char* text, struct DoxygenTag* tag_out) __attribute__((nonnull(1, 2)));

/**
 * @brief Aligns `@param` descriptions to the widest parameter name plus one space.
 *
 * Continuations of a tag are indented to that tag's description column. `@return` and `@brief` keep
 * their own description columns. The function rewrites line texts in place using `arena`.
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
