#ifndef CWRAP_COMMENT_H
#define CWRAP_COMMENT_H

#include <stdbool.h>
#include <stddef.h>

struct Arena;
struct CommentSpanList;
struct StringBuffer;

/** Block decoration, classified from continuation lines rather than from the opener. */
enum CommentShape {
  /** Consecutive `//` lines at the same indent. */
  COMMENT_SHAPE_LINE,

  /** Continuation lines carry a leading `*`. The closer sits on its own last line, except when the
     refilled prose leaves no continuation line, where it trails the opener. */
  COMMENT_SHAPE_STARRED,

  /** Continuations hang under the opener. The closer trails the last prose line. */
  COMMENT_SHAPE_HANGING,
};

/** One wrapable comment region. Trailing comments are not grouped. */
struct CommentBlock {
  /** Decoration that governs prefix and closer placement. */
  enum CommentShape shape;

  /** Byte offset of the first marker in the block. */
  size_t start;

  /** Byte offset immediately after the last byte of the block. */
  size_t end;

  /** Display columns of leading whitespace before the opener. */
  size_t indent_columns;

  /** Whether newly emitted lines use CRLF rather than LF. */
  bool has_crlf_newlines;

  /** Whether the opener line carries no prose, so it stays a marker-only line. */
  bool is_opener_empty;

  /** Whether the opener is a Doxygen slash-star-star marker. */
  bool is_doxygen_opener;
};

/** Arena-owned list of wrapable comment blocks in source order. */
struct CommentBlockList {
  /** Block array, or `NULL` when `count` is 0. */
  struct CommentBlock* items;

  /** Number of blocks in `items`. */
  size_t count;
};

/** Kind of one decoration-stripped body line. */
enum BodyLineKind {
  /** Ordinary prose that may be joined and refilled. */
  BODY_LINE_PROSE,

  /** A Doxygen tag line (`@brief`, `@param`, `@return`) that starts a new unit. */
  BODY_LINE_TAG,

  /** A list item, left untouched. */
  BODY_LINE_LIST,

  /** A fenced or indented sample, left untouched. */
  BODY_LINE_CODE,

  /** A decorative run of punctuation, left untouched. */
  BODY_LINE_DECORATION,

  /** An empty payload, which ends a paragraph. */
  BODY_LINE_BLANK,
};

/** One decoration-stripped line of comment body. */
struct BodyLine {
  /** How this line participates in wrapping. */
  enum BodyLineKind kind;

  /** Payload after markers are stripped. Arena-owned and terminated. */
  char* text;

  /** Number of bytes in `text`, excluding the terminator. */
  size_t text_len;
};

/** Arena-owned list of body lines. */
struct BodyLineList {
  /** Line array, or `NULL` when `count` is 0. */
  struct BodyLine* items;

  /** Number of lines in `items`. */
  size_t count;
};

/**
 * @brief Groups wrapable comment spans into blocks and classifies each block's decoration.
 *
 * Consecutive same-indent `//` comments with only a newline between them become one line block. A
 * trailing comment is omitted. A slash-star block is starred when a continuation `*` sits at the
 * decoration column, otherwise hanging. A `*` aligned under the prose is a list marker.
 *
 * @param source     Source bytes referenced by `spans`. Must not be `NULL` when `source_len` is
 *                   non-zero.
 * @param source_len Number of bytes in `source`.
 * @param spans      Comment spans from `lex_comment_spans`. Must not be `NULL`.
 * @param arena      Arena that owns the block array. Must not be `NULL`.
 * @param blocks_out Receives the block list. Written only on success. Must not be `NULL`.
 * @param err        Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len    Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int comment_group(const char* source,
                  size_t source_len,
                  const struct CommentSpanList* spans,
                  struct Arena* arena,
                  struct CommentBlockList* blocks_out,
                  char* err,
                  size_t err_len) __attribute__((nonnull(3, 4, 5)));

/**
 * @brief Strips decoration from `block` and classifies each remaining payload line.
 *
 * The closer is stripped before any `*` decoration, so a closer-only last line is dropped for both
 * starred and hanging blocks. Hanging continuations lose only the hanging indent, so extra indent
 * that marks a sample stays on the payload.
 *
 * @param source    Source bytes containing `block`. Must not be `NULL`.
 * @param block     Block whose body is extracted. Must not be `NULL`.
 * @param arena     Arena that owns the line texts and array. Must not be `NULL`.
 * @param lines_out Receives the line list. Written only on success. Must not be `NULL`.
 * @param err       Receives a diagnostic on failure. May be `NULL` only when `err_len` is 0.
 * @param err_len   Size of `err` in bytes.
 * @return `0` on success, or `-1` on allocation failure.
 */
int comment_extract_body(const char* source,
                         const struct CommentBlock* block,
                         struct Arena* arena,
                         struct BodyLineList* lines_out,
                         char* err,
                         size_t err_len) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Writes `block` reconstructed from `lines` onto `buffer`.
 *
 * Restores the opener, continuation prefixes, and closer required by `block->shape`.
 *
 * @param buffer Buffer that receives the reconstructed comment. Must not be `NULL`.
 * @param source Source bytes, used only to recover the original indent whitespace. Must not be
 *               `NULL`.
 * @param block  Block whose decoration is restored. Must not be `NULL`.
 * @param lines  Filled body lines. Must not be `NULL`.
 * @return `0` on success, or `-1` on allocation failure.
 */
int comment_emit(struct StringBuffer* buffer,
                 const char* source,
                 const struct CommentBlock* block,
                 const struct BodyLineList* lines) __attribute__((nonnull(1, 2, 3, 4)));

/**
 * @brief Returns the display-column width of the continuation prefix for `block`.
 *
 * @param block Block whose continuation prefix is measured. Must not be `NULL`.
 * @return Columns occupied by indent plus `// `, ` * `, or hanging spaces.
 */
size_t comment_prefix_columns(const struct CommentBlock* block) __attribute__((nonnull(1)));

/**
 * @brief Returns the display-column width before the first body line for `block`.
 *
 * An inline Doxygen opener occupies one more column than a continuation prefix. A marker-only
 * opener puts the first body line on a continuation and therefore uses the continuation width.
 *
 * @param block Block whose first body-line prefix is measured. Must not be `NULL`.
 * @return Columns occupied before the first body line.
 */
size_t comment_first_prefix_columns(const struct CommentBlock* block) __attribute__((nonnull(1)));

#endif
