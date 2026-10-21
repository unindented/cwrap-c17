// Seeded random tests over `cwrap`'s rewrap. Each property runs a fixed number of cases generated
// from `TEST_RNG_SEED_DEFAULT`, so the pass is deterministic, needs no fuzzing engine, and runs
// everywhere the suite does, under the sanitizers of whichever preset builds it. A failing check
// names the case. It reaches only the input shapes its generator lists.

#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "domain/lex.h"
#include "domain/rewrite.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/** Generated cases each rewrap property runs. */
enum { RANDOM_CASE_COUNT = 2000 };

/**
 * Widest column `--width` accepts. `WRAP_COLUMN_MAX` is file-local to `cli.c`, so the limit is
 * spelled here as a literal, as `test_cli.c` does. Changing `WRAP_COLUMN_MAX` must update it.
 */
enum { WIDTH_MAX = 10000 };

/** Most body lines one generated comment holds. */
enum { BODY_LINE_COUNT_MAX = 16 };

/**
 * @brief Returns one of `count` strings, chosen uniformly.
 *
 * @param rng     Generator to draw from. Must not be `NULL`.
 * @param choices Array of `count` strings. Must not be `NULL`.
 * @param count   Number of strings. Must be positive.
 * @return The chosen string.
 */
static struct Str pick(struct TestRng* rng, const struct Str* choices, ptrdiff_t count) {
  return choices[test_rng_below(rng, count)];
}

/**
 * @brief Appends between `append_count_min` and `append_count_max` tokens drawn from `tokens`,
 *        separated by single spaces.
 *
 * @param buffer           Buffer that receives the tokens. Must not be `NULL`.
 * @param rng              Generator to draw from. Must not be `NULL`.
 * @param tokens           Array of `token_count` tokens. Must not be `NULL`.
 * @param token_count      Number of tokens. Must be positive.
 * @param append_count_min Fewest tokens to append.
 * @param append_count_max Most tokens to append. Must not be below `append_count_min`.
 */
static void append_tokens(struct Buf* buffer,
                          struct TestRng* rng,
                          const struct Str* tokens,
                          ptrdiff_t token_count,
                          ptrdiff_t append_count_min,
                          ptrdiff_t append_count_max) {
  const ptrdiff_t append_count =
      append_count_min + test_rng_below(rng, append_count_max - append_count_min + 1);
  for (ptrdiff_t i = 0; i < append_count; i++) {
    if (i > 0) {
      buf_append_char(buffer, ' ');
    }
    buf_append(buffer, pick(rng, tokens, token_count));
  }
}

/**
 * @brief Appends between `append_count_min` and `append_count_max` words, each a long run of
 *        letters one time in twelve and otherwise drawn from a list of prose, wide characters,
 *        backtick spans, and the commands and markers that must travel with the word before them.
 *
 * Two shapes the rewrap gets wrong are left out. The list has no `@param` or `@retval`, because a
 * line that starts with either takes the next word as the name, and a name that begins a longer
 * atom, such as `E2 80 94` or a backtick span with a space, is written twice. And the last word is
 * never a lone `-` or `*`, which a refill can leave alone on a line, where it reads back as a
 * decoration line.
 *
 * @param buffer           Buffer that receives the words, separated by single spaces. Must not be
 *                         `NULL`.
 * @param rng              Generator to draw from. Must not be `NULL`.
 * @param append_count_min Fewest words to append.
 * @param append_count_max Most words to append. Must not be below `append_count_min`.
 */
static void append_words(struct Buf* buffer,
                         struct TestRng* rng,
                         ptrdiff_t append_count_min,
                         ptrdiff_t append_count_max) {
  static const struct Str words[] = {
      STR_INIT("one"),
      STR_INIT("two"),
      STR_INIT("a"),
      STR_INIT("the"),
      STR_INIT("wraps"),
      STR_INIT("end."),
      STR_INIT("list,"),
      STR_INIT("rng's"),
      STR_INIT("re-entry"),
      STR_INIT("(aside"),
      STR_INIT("note)"),
      STR_INIT("264 MB"),
      STR_INIT("E2 80 94"),
      STR_INIT("`a b`"),
      STR_INIT("`x`"),
      STR_INIT("`open"),
      STR_INIT("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"),
      STR_INIT("na\xC3\xAFve"),
      STR_INIT("e\xCC\x81t\xCC\x81"),
      STR_INIT("\xF0\x9F\x98\x80"),
      STR_INIT("@ref"),
      STR_INIT("\\c"),
      STR_INIT("-"),
      STR_INIT("*"),
      STR_INIT("1."),
      STR_INIT("NOLINT"),
      STR_INIT("http://x//y"),
  };
  const ptrdiff_t append_count =
      append_count_min + test_rng_below(rng, append_count_max - append_count_min + 1);
  for (ptrdiff_t i = 0; i < append_count; i++) {
    if (i > 0) {
      buf_append_char(buffer, ' ');
    }
    struct Str word = pick(rng, words, (ptrdiff_t)(sizeof(words) / sizeof(words[0])));
    if (i == append_count - 1 && (str_eq(word, S("-")) || str_eq(word, S("*")))) {
      word = S("end.");
    }
    if (test_rng_below(rng, 12) == 0) {
      const ptrdiff_t len = 16 + test_rng_below(rng, 105);
      for (ptrdiff_t j = 0; j < len; j++) {
        buf_append_char(buffer, (char)('a' + test_rng_below(rng, 26)));
      }
    } else {
      buf_append(buffer, word);
    }
  }
}

/**
 * @brief Builds one part of a comment body: a prose line, a Doxygen tag, a list item, a tool
 *        directive, a fenced sample, an indented sample, a decoration, or a blank line.
 *
 * @param rng       Generator to draw from. Must not be `NULL`.
 * @param arena     Arena that owns the line texts. Must not be `NULL`.
 * @param lines_out Receives the part's lines, at most 4. Must not be `NULL`.
 * @return The number of lines the part holds.
 */
static ptrdiff_t random_body_part(struct TestRng* rng, struct Arena* arena, struct Str* lines_out) {
  // Every tag that takes a name carries it, because the rewrap gets a bare `@param` whose name is
  // on the next line wrong: it aligns the parameter names before it joins that line, so the names
  // line up, and the continuation's inner spaces go, only on the second pass.
  static const struct Str tags[] = {
      STR_INIT("@brief"),     STR_INIT("@param x"), STR_INIT("@param[in] value"),
      STR_INIT("\\param y"),  STR_INIT("@return"),  STR_INIT("@retval 0"),
      STR_INIT("@retval -1"), STR_INIT("@note"),    STR_INIT("@see"),
  };
  static const struct Str markers[] = {STR_INIT("- "), STR_INIT("* "), STR_INIT("+ "),
                                       STR_INIT("1. "), STR_INIT("12. ")};
  static const struct Str directives[] = {
      STR_INIT("NOLINT"),
      STR_INIT("NOLINTNEXTLINE(bugprone-x)"),
      STR_INIT("NOLINTBEGIN(misc-no-recursion)"),
      STR_INIT("cppcheck-suppress id"),
      STR_INIT("clang-format off"),
      STR_INIT("clang-format on"),
      STR_INIT("IWYU pragma: keep"),
  };
  static const struct {
    struct Str opener;
    struct Str closer;
  } fences[] = {
      {STR_INIT("```"), STR_INIT("```")},
      {STR_INIT("```c"), STR_INIT("```")},
      {STR_INIT("~~~"), STR_INIT("~~~")},
      {STR_INIT("@code"), STR_INIT("@endcode")},
      {STR_INIT("@code{.c}"), STR_INIT("@endcode")},
      {STR_INIT("\\code"), STR_INIT("\\endcode")},
      {STR_INIT("@verbatim"), STR_INIT("@endverbatim")},
  };
  static const struct Str samples[] = {STR_INIT("int  x;"), STR_INIT("  keep   this"),
                                       STR_INIT("f(a,  b);")};
  static const struct Str decorations[] = {STR_INIT("-----"), STR_INIT("===="), STR_INIT("~~")};
  struct Buf line = {.arena = arena};
  const ptrdiff_t kind = test_rng_below(rng, 16);
  if (kind < 7) {
    append_words(&line, rng, 1, 10);
  } else if (kind < 9) {
    buf_append(&line, pick(rng, tags, (ptrdiff_t)(sizeof(tags) / sizeof(tags[0]))));
    if (test_rng_below(rng, 4) != 0) {
      buf_append_char(&line, ' ');
      append_words(&line, rng, 1, 10);
    }
  } else if (kind < 10) {
    buf_append(&line, pick(rng, markers, (ptrdiff_t)(sizeof(markers) / sizeof(markers[0]))));
    append_words(&line, rng, 1, 6);
  } else if (kind < 11) {
    buf_append(&line,
               pick(rng, directives, (ptrdiff_t)(sizeof(directives) / sizeof(directives[0]))));
  } else if (kind < 12) {
    // A fenced sample holds up to two lines, and one fence in eight is never closed.
    const ptrdiff_t fence = test_rng_below(rng, (ptrdiff_t)(sizeof(fences) / sizeof(fences[0])));
    ptrdiff_t count = 0;
    lines_out[count++] = fences[fence].opener;
    const ptrdiff_t sample_count = test_rng_below(rng, 3);
    for (ptrdiff_t i = 0; i < sample_count; i++) {
      lines_out[count++] = pick(rng, samples, (ptrdiff_t)(sizeof(samples) / sizeof(samples[0])));
    }
    if (test_rng_below(rng, 8) != 0) {
      lines_out[count++] = fences[fence].closer;
    }
    return count;
  } else if (kind < 13) {
    buf_append(&line, S("    "));
    buf_append(&line, pick(rng, samples, (ptrdiff_t)(sizeof(samples) / sizeof(samples[0]))));
  } else if (kind < 14) {
    buf_append(&line,
               pick(rng, decorations, (ptrdiff_t)(sizeof(decorations) / sizeof(decorations[0]))));
  }
  lines_out[0] = buf_str(&line);
  return 1;
}

/**
 * @brief Draws the body lines of one comment: one time in three two lines of one or two short
 *        words, which a narrow width often joins onto one line, or onto the opener line, exactly at
 *        its limit, and otherwise up to six random body parts.
 *
 * @param rng       Generator to draw from. Must not be `NULL`.
 * @param arena     Arena that owns the line texts. Must not be `NULL`.
 * @param lines_out Receives the lines, at most `BODY_LINE_COUNT_MAX`. Must not be `NULL`.
 * @return The number of lines drawn, at least 1.
 */
static ptrdiff_t append_comment_body(struct TestRng* rng,
                                     struct Arena* arena,
                                     struct Str* lines_out) {
  ptrdiff_t line_count = 0;
  if (test_rng_below(rng, 3) == 0) {
    static const struct Str short_words[] = {STR_INIT("a"), STR_INIT("one"), STR_INIT("two"),
                                             STR_INIT("the"), STR_INIT("end.")};
    for (; line_count < 2; line_count++) {
      struct Buf line = {.arena = arena};
      append_tokens(&line, rng, short_words,
                    (ptrdiff_t)(sizeof(short_words) / sizeof(short_words[0])), 1, 2);
      lines_out[line_count] = buf_str(&line);
    }
    return line_count;
  }
  const ptrdiff_t part_count = 1 + test_rng_below(rng, 6);
  for (ptrdiff_t i = 0; i < part_count && line_count + 4 <= BODY_LINE_COUNT_MAX; i++) {
    line_count += random_body_part(rng, arena, lines_out + line_count);
  }
  return line_count;
}

/**
 * @brief Appends `lines` as a run of `//`, `///` or `//!` lines.
 *
 * @param buffer     Buffer that receives the run and the line ending of each line. Must not be
 *                   `NULL`.
 * @param rng        Generator to draw from. Must not be `NULL`.
 * @param indent     Indent of every line.
 * @param lines      Array of `line_count` body lines. Must not be `NULL`.
 * @param line_count Number of lines.
 * @param line_end   Line ending of the file, `\n` or `\r\n`.
 */
static void append_comment_line_run(struct Buf* buffer,
                                    struct TestRng* rng,
                                    struct Str indent,
                                    const struct Str* lines,
                                    ptrdiff_t line_count,
                                    struct Str line_end) {
  static const struct Str line_openers[] = {STR_INIT("//"), STR_INIT("///"), STR_INIT("//!")};
  const struct Str opener =
      pick(rng, line_openers, (ptrdiff_t)(sizeof(line_openers) / sizeof(line_openers[0])));
  for (ptrdiff_t i = 0; i < line_count; i++) {
    buf_append(buffer, indent);
    buf_append(buffer, opener);
    if (lines[i].len > 0) {
      buf_append_char(buffer, ' ');
      buf_append(buffer, lines[i]);
    }
    buf_append(buffer, line_end);
  }
}

/**
 * @brief Appends `lines` as a slash-star block with any of the three openers, in starred, hanging
 *        or one-line shape.
 *
 * @param buffer     Buffer that receives the block and its final line ending. Must not be `NULL`.
 * @param rng        Generator to draw from. Must not be `NULL`.
 * @param indent     Indent of the opener line and of the lines below it.
 * @param lines      Array of `line_count` body lines. Must not be `NULL`.
 * @param line_count Number of lines. Must be positive.
 * @param line_end   Line ending of the file, `\n` or `\r\n`.
 */
static void append_comment_block(struct Buf* buffer,
                                 struct TestRng* rng,
                                 struct Str indent,
                                 const struct Str* lines,
                                 ptrdiff_t line_count,
                                 struct Str line_end) {
  static const struct Str block_openers[] = {STR_INIT("/*"), STR_INIT("/**"), STR_INIT("/*!")};
  const struct Str opener =
      pick(rng, block_openers, (ptrdiff_t)(sizeof(block_openers) / sizeof(block_openers[0])));
  buf_append(buffer, indent);
  buf_append(buffer, opener);
  switch (test_rng_below(rng, 3)) {
    case 0: {
      // Starred, with prose on the opener line half the time.
      ptrdiff_t first = 0;
      if (test_rng_below(rng, 2) == 0 && lines[0].len > 0) {
        buf_append_char(buffer, ' ');
        buf_append(buffer, lines[0]);
        first = 1;
      }
      buf_append(buffer, line_end);
      for (ptrdiff_t i = first; i < line_count; i++) {
        buf_append(buffer, indent);
        buf_append(buffer, lines[i].len > 0 ? S(" * ") : S(" *"));
        buf_append(buffer, lines[i]);
        buf_append(buffer, line_end);
      }
      buf_append(buffer, indent);
      buf_append(buffer, S(" */"));
      break;
    }
    case 1:
      // Hanging under the opener, with the closer after the last line. That line is never blank:
      // the rewrap gives a hanging block that ends in a blank line and a closer line a starred
      // closer, which the next pass reads as a starred block and joins onto the opener.
      while (line_count > 1 && lines[line_count - 1].len == 0) {
        line_count--;
      }
      for (ptrdiff_t i = 0; i < line_count; i++) {
        if (i > 0) {
          buf_append(buffer, line_end);
          buf_append(buffer, indent);
          for (ptrdiff_t j = 0; j <= opener.len; j++) {
            buf_append_char(buffer, ' ');
          }
        } else {
          buf_append_char(buffer, ' ');
        }
        buf_append(buffer, lines[i]);
      }
      buf_append(buffer, S(" */"));
      break;
    default:
      buf_append_char(buffer, ' ');
      buf_append(buffer, lines[0]);
      buf_append(buffer, S(" */"));
      break;
  }
  buf_append(buffer, line_end);
}

/**
 * @brief Appends one comment: a run of `//`, `///` or `//!` lines, or a slash-star block with any
 *        of the three openers in starred, hanging or one-line shape, at a random indent.
 *
 * @param buffer   Buffer that receives the comment and its final line ending. Must not be `NULL`.
 * @param rng      Generator to draw from. Must not be `NULL`.
 * @param line_end Line ending of the file, `\n` or `\r\n`.
 * @param scratch  Scratch for the body lines, outside the region `buffer` grows in.
 */
static void append_comment(struct Buf* buffer,
                           struct TestRng* rng,
                           struct Str line_end,
                           struct Arena scratch) {
  static const struct Str indents[] = {STR_INIT(""), STR_INIT("  "), STR_INIT("    "),
                                       STR_INIT("\t"), STR_INIT("\t  ")};
  struct Str lines[BODY_LINE_COUNT_MAX];
  const ptrdiff_t line_count = append_comment_body(rng, &scratch, lines);
  const struct Str indent = pick(rng, indents, (ptrdiff_t)(sizeof(indents) / sizeof(indents[0])));
  if (test_rng_below(rng, 2) == 0) {
    append_comment_line_run(buffer, rng, indent, lines, line_count, line_end);
  } else {
    append_comment_block(buffer, rng, indent, lines, line_count, line_end);
  }
}

/**
 * @brief Returns a random source file: lines of code, trailing comments and comments to rewrap, in
 *        LF or CRLF lines, with or without a final line ending.
 *
 * @param rng     Generator to draw from. Must not be `NULL`.
 * @param perm    Arena that owns the source. Must not be `NULL`.
 * @param scratch Scratch for the generator's temporaries, outside `perm`'s region.
 * @return The source.
 */
static struct Str random_source(struct TestRng* rng, struct Arena* perm, struct Arena scratch) {
  static const struct Str code[] = {
      STR_INIT("int x = 1;"),
      STR_INIT("char* u = \"http://x//y\";"),
      STR_INIT("char c = '/';"),
      STR_INIT("#include \"a//b.h\""),
      STR_INIT("x = a / b * c;"),
      STR_INIT("  return 0;"),
      STR_INIT("}"),
      STR_INIT(""),
      STR_INIT("int y; // a trailing note that is long enough to wrap"),
      STR_INIT("int z; /* a trailing block note */"),
      STR_INIT("/* a */ /* b c d e f g */"),
      STR_INIT("#define M(a) \\"),
  };
  struct Buf buffer = {.arena = perm};
  const struct Str line_end = test_rng_below(rng, 4) == 0 ? S("\r\n") : S("\n");
  const ptrdiff_t item_count = 1 + test_rng_below(rng, 6);
  bool is_code_last = false;
  for (ptrdiff_t i = 0; i < item_count; i++) {
    is_code_last = test_rng_below(rng, 3) == 0;
    if (is_code_last) {
      buf_append(&buffer, pick(rng, code, (ptrdiff_t)(sizeof(code) / sizeof(code[0]))));
      buf_append(&buffer, line_end);
    } else {
      append_comment(&buffer, rng, line_end, scratch);
    }
  }
  struct Str source = buf_str(&buffer);
  // A CRLF file ends without its line ending only after a line of code: the rewrap breaks a comment
  // on an unterminated last line with LF, whatever the file's other lines end with.
  if (test_rng_below(rng, 8) == 0 && (line_end.len == 1 || is_code_last)) {
    source = str_head(source, source.len - line_end.len);
  }
  // Every input this file hands the code under test is copied into an allocation of exactly its
  // length, so the address sanitizer reports a read past its end, which the spare capacity of a
  // `Buf` would hide.
  return str_clone(source, perm);
}

/**
 * @brief Returns a random wrapping column: from the whole range one time in eight, up to 120 three
 *        times in eight, and otherwise up to 40, so most generated comments wrap and a short one
 *        often meets its limit.
 *
 * @param rng Generator to draw from. Must not be `NULL`.
 * @return A column from 1 to `WIDTH_MAX`.
 */
static ptrdiff_t random_width(struct TestRng* rng) {
  const ptrdiff_t range = test_rng_below(rng, 8);
  return 1 + test_rng_below(rng, range == 0 ? WIDTH_MAX : range < 4 ? 120 : 40);
}

/**
 * @brief Rewraps `source` at `width`.
 *
 * @param source  Source bytes.
 * @param width   Wrapping column. Must be at least 1.
 * @param perm    Arena that owns the result. Must not be `NULL`.
 * @param scratch Scratch for the rewrite's temporaries, outside `perm`'s region.
 * @return The rewrapped source, copied as `random_source` copies its source, because the
 *         idempotence property rewraps it again.
 */
static struct Str rewrap(struct Str source,
                         ptrdiff_t width,
                         struct Arena* perm,
                         struct Arena scratch) {
  struct Buf out = {.arena = perm};
  rewrite_source(&out, source, width, scratch);
  return str_clone(buf_str(&out), perm);
}

// A random file at a random width rewraps to a fixed point: a second pass at the same width changes
// nothing.
static void test_rewrap_is_idempotent(void) {
  struct TestRng rng = {TEST_RNG_SEED_DEFAULT};
  const struct Arena block = test_arena();
  const struct Arena scratch_block = test_arena();
  for (ptrdiff_t i = 0; i < RANDOM_CASE_COUNT; i++) {
    struct Arena arena = block;
    const struct Str source = random_source(&rng, &arena, scratch_block);
    const ptrdiff_t width = random_width(&rng);
    const struct Str once = rewrap(source, width, &arena, scratch_block);
    const struct Str twice = rewrap(once, width, &arena, scratch_block);
    if (!TEST_CHECK_(str_eq(once, twice), "source %td rewraps to a fixed point", i)) {
      const struct Str source_text = test_printable(source, &arena);
      TEST_MSG("width %td, source: '%.*s'", width, STR_FMT(source_text));
      const struct Str once_text = test_printable(once, &arena);
      TEST_MSG("once: '%.*s'", STR_FMT(once_text));
      const struct Str twice_text = test_printable(twice, &arena);
      TEST_MSG("twice: '%.*s'", STR_FMT(twice_text));
      break;
    }
  }
}

/**
 * @brief Returns the line ending of `source`: `\r\n` when its first line ends so, and otherwise
 *        `\n`.
 *
 * @param source Source bytes, whose lines all end the same way.
 * @return The line ending.
 */
static struct Str file_line_end(struct Str source) {
  for (ptrdiff_t i = 0; i < source.len; i++) {
    if (source.data[i] == '\n') {
      return i > 0 && source.data[i - 1] == '\r' ? S("\r\n") : S("\n");
    }
  }
  return S("\n");
}

/**
 * @brief Returns the indent and opener of a line comment that starts its line, which a refill
 *        repeats on every line of its `//` run, or an empty string for any other comment.
 *
 * @param source Source bytes.
 * @param span   Comment in `source`. Must not be `NULL`.
 * @return The indent and opener, borrowed from `source`.
 */
static struct Str line_run_prefix(struct Str source, const struct CommentSpan* span) {
  if (span->kind != COMMENT_KIND_LINE || span->is_trailing) {
    return (struct Str){0};
  }
  ptrdiff_t line_start = span->start;
  while (line_start > 0 &&
         (source.data[line_start - 1] == ' ' || source.data[line_start - 1] == '\t')) {
    line_start--;
  }
  ptrdiff_t opener_end = span->start + 2;
  if (opener_end < span->end &&
      (source.data[opener_end] == '/' || source.data[opener_end] == '!')) {
    opener_end++;
  }
  return (struct Str){source.data + line_start, opener_end - line_start};
}

/**
 * @brief Returns `source` with its comments cut out, along with each line break between two lines
 *        of one `//` run, so what remains is what a rewrap must not touch.
 *
 * Two line comments are lines of one run when each starts its line with the same indent and opener
 * and one `line_end` separates them. A refill adds and removes such breaks, and nothing else
 * between two comments: a break after a trailing comment, a blank line, a break between two runs or
 * into a block, and a break in the other line ending all stay.
 *
 * @param source   Source bytes.
 * @param line_end Line ending of the source the rewrap read, `\n` or `\r\n`.
 * @param arena    Arena that owns the result. Must not be `NULL`.
 * @return The bytes outside comments.
 */
static struct Str code_bytes(struct Str source, struct Str line_end, struct Arena* arena) {
  const struct CommentSpanList spans = lex_comment_spans(source, arena);
  struct Buf code = {.arena = arena};
  ptrdiff_t cursor = 0;
  struct Str previous_prefix = {0};
  for (ptrdiff_t i = 0; i < spans.count; i++) {
    const struct Str gap = {source.data + cursor, spans.items[i].start - cursor};
    const struct Str prefix = line_run_prefix(source, &spans.items[i]);
    const bool is_run_break = prefix.len > 0 && str_eq(prefix, previous_prefix) &&
                              str_eq((struct Str){gap.data, prefix.data - gap.data}, line_end);
    if (!is_run_break) {
      buf_append(&code, gap);
    }
    previous_prefix = prefix;
    cursor = spans.items[i].end;
  }
  buf_append(&code, str_tail(source, cursor));
  return buf_str(&code);
}

// Rewrapping a random file leaves every byte outside its comments as it was, apart from the line
// breaks a refill moves between the lines of one `//` run.
static void test_rewrap_keeps_code_bytes(void) {
  struct TestRng rng = {TEST_RNG_SEED_DEFAULT};
  const struct Arena block = test_arena();
  const struct Arena scratch_block = test_arena();
  for (ptrdiff_t i = 0; i < RANDOM_CASE_COUNT; i++) {
    struct Arena arena = block;
    const struct Str source = random_source(&rng, &arena, scratch_block);
    const ptrdiff_t width = random_width(&rng);
    const struct Str out = rewrap(source, width, &arena, scratch_block);
    const struct Str line_end = file_line_end(source);
    const struct Str expected = code_bytes(source, line_end, &arena);
    const struct Str actual = code_bytes(out, line_end, &arena);
    if (!TEST_CHECK_(str_eq(actual, expected), "source %td keeps its code bytes", i)) {
      const struct Str source_text = test_printable(source, &arena);
      TEST_MSG("width %td, source: '%.*s'", width, STR_FMT(source_text));
      const struct Str out_text = test_printable(out, &arena);
      TEST_MSG("rewrapped: '%.*s'", STR_FMT(out_text));
      break;
    }
  }
}

TEST_LIST = {
    {"rewrap is idempotent", test_rewrap_is_idempotent},
    {"rewrap keeps code bytes", test_rewrap_keeps_code_bytes},
    {NULL, NULL},
};
