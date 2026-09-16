#include <acutest.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/string_buffer.h"
#include "domain/rewrite.h"

/**
 * @brief Rewrites source text at a requested width.
 *
 * @param source Terminated source text to rewrite.
 * @param width  Target display width in columns.
 * @return An allocated rewritten string the caller must free, or `NULL` on failure.
 */
static char* rewrite(const char* source, size_t width) {
  struct Arena arena;
  arena_init(&arena);
  struct StringBuffer out;
  string_buffer_init(&out);
  char err[128];
  TEST_CHECK(rewrite_source(&out, source, strlen(source), width, &arena, err, sizeof(err)) == 0);
  char* stolen = string_buffer_steal(&out);
  arena_free(&arena);
  return stolen;
}

// Code and a string containing slashes are copied unchanged.
static void test_leaves_code_and_strings_unchanged(void) {
  const char* source = "char* u = \"http://x//y\";\nint x = 1;\n";
  char* out = rewrite(source, 40);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// A CRLF-spliced string remains byte-identical and its continued `//` is not rewritten.
static void test_leaves_crlf_spliced_string_unchanged(void) {
  const char* source = "const char* u = \"http:\\\r\n//example.com\";\r\n";
  char* out = rewrite(source, 40);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// A trailing comment after code is copied unchanged.
static void test_leaves_trailing_comment_unchanged(void) {
  const char* source = "int x; /* do not wrap this trailing note */\n";
  char* out = rewrite(source, 20);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// A block comment after a code-line splice is trailing on the logical line and stays unchanged.
static void test_leaves_spliced_trailing_comment_unchanged(void) {
  const char* source = "#define X \\\n  /* one two three four five six */\n";
  char* out = rewrite(source, 15);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// Short line-comment lines are joined.
static void test_joins_short_line_comments(void) {
  const char* source = "// one\n// two\n";
  char* out = rewrite(source, 40);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "// one two\n") == 0);
  free(out);
}

// A width consumed by the comment prefix still puts later atoms on separate best-effort lines.
static void test_narrow_width_splits_atoms(void) {
  const char* source = "// one two three\n";
  char* out = rewrite(source, 1);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "// one\n// two\n// three\n") == 0);
  free(out);
}

// Supplementary-plane wide characters consume two columns when deciding a line break.
static void test_wraps_wide_emoji_by_display_columns(void) {
  const char* source = "// 😀 😀\n";
  char* out = rewrite(source, 6);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "// 😀\n// 😀\n") == 0);
  free(out);
}

// A decomposed wide character keeps its zero-width mark on a line that fits exactly.
static void test_wraps_combining_mark_by_display_columns(void) {
  const char* source = "// が x\n";
  char* out = rewrite(source, 7);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// Rewritten comment lines retain the source's CRLF convention.
static void test_preserves_crlf_line_endings(void) {
  const char* source = "// one two three four\r\nint x;\r\n";
  const char* expected = "// one\r\n// two\r\n// three\r\n// four\r\nint x;\r\n";
  char* out = rewrite(source, 8);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// Starred and hanging block comments also retain CRLF when filling creates new lines.
static void test_preserves_crlf_in_block_shapes(void) {
  const char* starred_source = "/**\r\n * one two\r\n */\r\n";
  const char* starred_expected = "/**\r\n * one\r\n * two\r\n */\r\n";
  char* out = rewrite(starred_source, 8);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, starred_expected) == 0);
  free(out);

  const char* hanging_source = "/* one two */\r\n";
  const char* hanging_expected = "/* one\r\n   two */\r\n";
  out = rewrite(hanging_source, 8);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, hanging_expected) == 0);
  free(out);
}

// An inline Doxygen opener consumes four columns when deciding the first break.
static void test_counts_inline_doxygen_opener_width(void) {
  char* out = rewrite("/** a b */\n", 9);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "/** a\n   b */\n") == 0);
  free(out);
}

// A word beginning with a tag spelling is ordinary prose unless the tag has a token boundary.
static void test_tag_prefix_word_stays_prose(void) {
  const char* source = "/**\n * first line\n * @briefly second line\n */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "/**\n * first line @briefly second line\n */\n") == 0);
  free(out);
}

// Direction-qualified parameter tags are classified, aligned, and retained through filling.
static void test_aligns_direction_parameters_end_to_end(void) {
  const char* source =
      "/**\n"
      " * @param[in] x first\n"
      " * @param[out] longer second\n"
      " */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  const char* expected =
      "/**\n"
      " * @param[in] x      first\n"
      " * @param[out] longer second\n"
      " */\n";
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// Wrapped line-comment continuations reuse the original indent.
static void test_reindents_wrapped_line_comments(void) {
  const char* source = "  // one two three four five six\n";
  char* out = rewrite(source, 16);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "  // one two\n  // three four\n  // five six\n") == 0);
  free(out);
}

// A blank line inside a hanging block is a bare newline, not a line of indent spaces.
static void test_hanging_blank_line_has_no_trailing_spaces(void) {
  const char* source = "/* First paragraph.\n\n   Second paragraph. */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "/* First paragraph.\n\n   Second paragraph. */\n") == 0);
  free(out);
}

// A hyphen glued to a backtick span is not given a space.
static void test_preserves_hyphen_after_backtick(void) {
  const char* source =
      "  /* Payload region: `capacity` bytes, `max_align_t`-aligned so any type fits. */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// An indented hanging comment keeps the original indent. Emit does not write it again.
static void test_preserves_indent_on_hanging_comment(void) {
  const char* source =
      "  /* Combining marks sit on the previous character and occupy no extra column. */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// `be able` is two words, so a hanging line can keep `be` when it still fits.
static void test_keeps_be_on_line_when_it_fits(void) {
  const char* source =
      "  /* The whole element trails the cause. It is a raw argument, so it is unbounded and must "
      "not be\n"
      "     able to truncate the reason away. */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// A parenthetical wraps at its spaces instead of moving to the next line as one unit.
static void test_breaks_inside_parenthetical(void) {
  const char* source =
      "/**\n"
      " * Splits the payload at `atom` boundaries and keeps a backtick span whole (so a span never "
      "straddles a break), then reflows the rest.\n"
      " */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  const char* expected =
      "/**\n"
      " * Splits the payload at `atom` boundaries and keeps a backtick span whole (so a span "
      "never\n"
      " * straddles a break), then reflows the rest.\n"
      " */\n";
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// A hanging closer-only last line is rewritten as a trailing closer on the last prose line.
static void test_hanging_closer_trails_last_prose(void) {
  const char* source = "  /* one two\n     three\n   */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, "  /* one two three */\n") == 0);
  free(out);
}

// A list-item continuation keeps the two-space hanging indent under the marker.
static void test_preserves_list_continuation_indent(void) {
  const char* source =
      "/**\n"
      " * - an unquoted attribute value (whitespace, `=` and a backtick pass through, so the value "
      "can end\n"
      " *   early and inject an attribute)\n"
      " */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  TEST_CHECK(strcmp(out, source) == 0);
  free(out);
}

// A punctuation-only backtick span stays prose and travels with the word before it.
static void test_joins_leading_with_backtick_slash(void) {
  const char* source =
      "/**\n"
      " * The output-relative path is the returned URL without its leading\n"
      " * `/`.\n"
      " */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  const char* expected =
      "/**\n"
      " * The output-relative path is the returned URL without its leading `/`.\n"
      " */\n";
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// A hanging `*` list is not treated as starred decoration and is not joined into prose.
static void test_leaves_hanging_star_list(void) {
  const char* source = "/* Keep:\n   * first item\n   * second item\n */\n";
  char* out = rewrite(source, 40);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  const char* expected = "/* Keep:\n   * first item\n   * second item */\n";
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// An indented sample in a hanging block is not joined into one prose line.
static void test_leaves_hanging_indented_sample(void) {
  const char* source =
      "/* line 3:\n"
      "\n"
      "     1  +++\n"
      "     2  title = \"Title\"\n"
      "     3  date = bad\n"
      "     4  +++\n"
      " */\n";
  char* out = rewrite(source, 100);
  TEST_ASSERT(out != NULL);
  if (out == NULL) {
    return;
  }
  const char* expected =
      "/* line 3:\n"
      "\n"
      "     1  +++\n"
      "     2  title = \"Title\"\n"
      "     3  date = bad\n"
      "     4  +++ */\n";
  TEST_CHECK(strcmp(out, expected) == 0);
  free(out);
}

// Width 0 is rejected with a diagnostic.
static void test_rejects_zero_width(void) {
  struct Arena arena;
  arena_init(&arena);
  struct StringBuffer out;
  string_buffer_init(&out);
  char err[128];
  TEST_CHECK(rewrite_source(&out, "int x;\n", 7, 0, &arena, err, sizeof(err)) == -1);
  TEST_CHECK(strcmp(err, "wrapping column must be a positive integer") == 0);
  string_buffer_free(&out);
  arena_free(&arena);
}

TEST_LIST = {
    {"leaves code and strings unchanged", test_leaves_code_and_strings_unchanged},
    {"leaves crlf spliced string unchanged", test_leaves_crlf_spliced_string_unchanged},
    {"leaves trailing comment unchanged", test_leaves_trailing_comment_unchanged},
    {"leaves spliced trailing comment unchanged", test_leaves_spliced_trailing_comment_unchanged},
    {"joins short line comments", test_joins_short_line_comments},
    {"narrow width splits atoms", test_narrow_width_splits_atoms},
    {"wraps wide emoji by display columns", test_wraps_wide_emoji_by_display_columns},
    {"wraps combining mark by display columns", test_wraps_combining_mark_by_display_columns},
    {"preserves crlf line endings", test_preserves_crlf_line_endings},
    {"preserves crlf in block shapes", test_preserves_crlf_in_block_shapes},
    {"counts inline doxygen opener width", test_counts_inline_doxygen_opener_width},
    {"tag prefix word stays prose", test_tag_prefix_word_stays_prose},
    {"aligns direction parameters end to end", test_aligns_direction_parameters_end_to_end},
    {"reindents wrapped line comments", test_reindents_wrapped_line_comments},
    {"hanging blank line has no trailing spaces", test_hanging_blank_line_has_no_trailing_spaces},
    {"preserves hyphen after backtick", test_preserves_hyphen_after_backtick},
    {"preserves indent on hanging comment", test_preserves_indent_on_hanging_comment},
    {"keeps be on line when it fits", test_keeps_be_on_line_when_it_fits},
    {"breaks inside parenthetical", test_breaks_inside_parenthetical},
    {"hanging closer trails last prose", test_hanging_closer_trails_last_prose},
    {"preserves list continuation indent", test_preserves_list_continuation_indent},
    {"joins leading with backtick slash", test_joins_leading_with_backtick_slash},
    {"leaves hanging star list", test_leaves_hanging_star_list},
    {"leaves hanging indented sample", test_leaves_hanging_indented_sample},
    {"rejects zero width", test_rejects_zero_width},
    {NULL, NULL},
};
