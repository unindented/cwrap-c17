#include <acutest.h>
#include <stddef.h>

#include "domain/rewrite.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Rewrites source text at a requested width.
 *
 * @param source Terminated source text to rewrite.
 * @param width  Target display width in columns.
 * @return The rewritten source, in an arena of its own.
 */
static struct Str rewrite(const char* source, ptrdiff_t width) {
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  rewrite_source(&out, str_from_cstr(source), width, test_arena());
  return buf_str(&out);
}

/** Bytes of the scratch region `rewrite_in_tiny_scratch` gives, too few for the comment spans. */
enum { SCRATCH_SIZE = 64 };

/**
 * @brief Rewrites a two-line comment with a scratch region too small for its comment spans. Run
 *        through `test_is_oom`, which calls it in a child process.
 *
 * @param userdata Unused.
 */
static void rewrite_in_tiny_scratch(void* userdata) {
  (void)userdata;
  static _Alignas(max_align_t) char memory[SCRATCH_SIZE];
  const struct Arena scratch = {.beg = memory, .end = memory + sizeof(memory)};
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  rewrite_source(&out, S("// one\n// two\n"), 40, scratch);
}

// Code and a string containing slashes are copied unchanged.
static void test_leaves_code_and_strings_unchanged(void) {
  const char* source = "char* u = \"http://x//y\";\nint x = 1;\n";
  const struct Str out = rewrite(source, 40);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A CRLF-spliced string remains byte-identical and its continued `//` is not rewritten.
static void test_leaves_crlf_spliced_string_unchanged(void) {
  const char* source = "const char* u = \"http:\\\r\n//example.com\";\r\n";
  const struct Str out = rewrite(source, 40);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A trailing comment after code is copied unchanged.
static void test_leaves_trailing_comment_unchanged(void) {
  const char* source = "int x; /* do not wrap this trailing note */\n";
  const struct Str out = rewrite(source, 20);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A block comment after a code-line splice is trailing on the logical line and stays unchanged.
static void test_leaves_spliced_trailing_comment_unchanged(void) {
  const char* source = "#define X \\\n  /* one two three four five six */\n";
  const struct Str out = rewrite(source, 15);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A comment after another comment on the same line is trailing and stays unchanged.
static void test_leaves_comment_after_comment_unchanged(void) {
  const char* source =
      "/* a */ /* one two three four five */\n/* b */ // one two three four five\n";
  const struct Str out = rewrite(source, 20);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A slash-star inside an angle-bracketed include path opens a comment that is trailing, so it and
// the comments it swallows up to the next star-slash are copied unchanged.
static void test_leaves_comment_opened_in_include_path_unchanged(void) {
  const char* source = "#include <c/*d.h>\n// one two three four five six\nint x; /* e */\n";
  const struct Str out = rewrite(source, 15);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A line comment continued by an LF or CRLF splice, and the code after it, stay unchanged.
static void test_leaves_spliced_line_comment_unchanged(void) {
  const char* sources[] = {
      "// note \\\n/* still the line comment above\nint x; /* real */\n",
      "// note \\\r\n/* still the line comment above\r\nint x; /* real */\r\n",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(sources) / sizeof(sources[0])); i++) {
    const struct Str out = rewrite(sources[i], 20);
    TEST_CHECK(str_eq(out, str_from_cstr(sources[i])));
  }
}

// Short line-comment lines are joined.
static void test_joins_short_line_comments(void) {
  const char* source = "// one\n// two\n";
  const struct Str out = rewrite(source, 40);
  TEST_CHECK(str_eq(out, S("// one two\n")));
}

// A width consumed by the comment prefix still puts later atoms on separate best-effort lines.
static void test_narrow_width_splits_atoms(void) {
  const char* source = "// one two three\n";
  const struct Str out = rewrite(source, 1);
  TEST_CHECK(str_eq(out, S("// one\n// two\n// three\n")));
}

// Supplementary-plane wide characters consume two columns when deciding a line break.
static void test_wraps_wide_emoji_by_display_columns(void) {
  const char* source = "// 😀 😀\n";
  const struct Str out = rewrite(source, 6);
  TEST_CHECK(str_eq(out, S("// 😀\n// 😀\n")));
}

// A decomposed wide character keeps its zero-width mark on a line that fits exactly.
static void test_wraps_combining_mark_by_display_columns(void) {
  const char* source = "// が x\n";
  const struct Str out = rewrite(source, 7);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// Rewritten comment lines retain the source's CRLF convention.
static void test_preserves_crlf_line_endings(void) {
  const char* source = "// one two three four\r\nint x;\r\n";
  const char* expected = "// one\r\n// two\r\n// three\r\n// four\r\nint x;\r\n";
  const struct Str out = rewrite(source, 8);
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// Starred and hanging block comments also retain CRLF when filling creates new lines.
static void test_preserves_crlf_in_block_shapes(void) {
  const char* starred_source = "/**\r\n * one two\r\n */\r\n";
  const char* starred_expected = "/**\r\n * one\r\n * two\r\n */\r\n";
  struct Str out = rewrite(starred_source, 8);
  TEST_CHECK(str_eq(out, str_from_cstr(starred_expected)));

  const char* hanging_source = "/* one two */\r\n";
  const char* hanging_expected = "/* one\r\n   two */\r\n";
  out = rewrite(hanging_source, 8);
  TEST_CHECK(str_eq(out, str_from_cstr(hanging_expected)));
}

// An inline Doxygen opener consumes four columns when deciding the first break, and a hanging
// continuation lines up under the text after it.
static void test_counts_inline_doxygen_opener_width(void) {
  const struct Str out = rewrite("/** a b */\n", 9);
  TEST_CHECK(str_eq(out, S("/** a\n    b */\n")));
}

// A tag spelling in a plain slash-star block is ordinary prose and joins the line above.
static void test_tag_in_plain_block_stays_prose(void) {
  const char* source = "/*\n * first line\n * @brief second line\n */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(out, S("/*\n * first line @brief second line\n */\n")));
}

// Direction-qualified parameter tags are classified, aligned, and retained through filling.
static void test_aligns_direction_parameters_end_to_end(void) {
  const char* source =
      "/**\n"
      " * @param[in] x first\n"
      " * @param[out] longer second\n"
      " */\n";
  const struct Str out = rewrite(source, 100);
  const char* expected =
      "/**\n"
      " * @param[in] x      first\n"
      " * @param[out] longer second\n"
      " */\n";
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// A tag name in a backtick span with a space in it, or a hex run, is one name, written once with
// its description aligned and hanging after it.
static void test_tag_name_atom_written_once(void) {
  const char* source =
      "/// @param `a b` the value.\n/// @param `cd`  the other.\n/// @retval FD 10 x\n"
      "/// @retval EOF   y\n";
  TEST_CHECK(str_eq(rewrite(source, 100), str_from_cstr(source)));
  TEST_CHECK(str_eq(rewrite("/// @param `a b` one two three\n", 26),
                    S("/// @param `a b` one two\n///              three\n")));
}

// Wrapped line-comment continuations reuse the original indent.
static void test_reindents_wrapped_line_comments(void) {
  const char* source = "  // one two three four five six\n";
  const struct Str out = rewrite(source, 16);
  TEST_CHECK(str_eq(out, S("  // one two\n  // three four\n  // five six\n")));
}

// A blank line inside a hanging block is a bare newline, not a line of indent spaces.
static void test_hanging_blank_line_has_no_trailing_spaces(void) {
  const char* source = "/* First paragraph.\n\n   Second paragraph. */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(out, S("/* First paragraph.\n\n   Second paragraph. */\n")));
}

// A hyphen glued to a backtick span is not given a space when the refill joins its lines.
static void test_preserves_hyphen_after_backtick(void) {
  const char* source =
      "  /* Payload region: `capacity` bytes, `max_align_t`-aligned\n     so any type fits. */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(
      out,
      S("  /* Payload region: `capacity` bytes, `max_align_t`-aligned so any type fits. */\n")));
}

// An indented hanging comment keeps the original indent when it is refilled. Emit does not write it
// again.
static void test_preserves_indent_on_hanging_comment(void) {
  const char* source =
      "  /* Combining marks sit on the previous character\n     and occupy no extra column. */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(
      out,
      S("  /* Combining marks sit on the previous character and occupy no extra column. */\n")));
}

// `be able` is two words, so a hanging line can keep `be` when it still fits.
static void test_keeps_be_on_line_when_it_fits(void) {
  const char* source =
      "  /* The whole element trails the cause. It is a raw argument, so it is unbounded and must "
      "not be\n"
      "     able to truncate the reason away. */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A parenthetical wraps at its spaces instead of moving to the next line as one unit.
static void test_breaks_inside_parenthetical(void) {
  const char* source =
      "/**\n"
      " * Splits the payload at `atom` boundaries and keeps a backtick span whole (so a span never "
      "straddles a break), then reflows the rest.\n"
      " */\n";
  const struct Str out = rewrite(source, 100);
  const char* expected =
      "/**\n"
      " * Splits the payload at `atom` boundaries and keeps a backtick span whole (so a span "
      "never\n"
      " * straddles a break), then reflows the rest.\n"
      " */\n";
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// A slash-star comment on one line that fits the column keeps its exact bytes. One that passes the
// column, spans lines, or holds a tab is refilled, and so is a line comment.
// `tests/fixtures/same-line-comments.c` covers a padded label on its own line.
static void test_one_line_block_within_width_unchanged(void) {
  const char* source = "/* one   two   */\n/*foo*/\n";
  TEST_CHECK(str_eq(rewrite(source, 40), str_from_cstr(source)));
  TEST_CHECK(str_eq(rewrite("/* one two three      */\n", 20), S("/* one two three */\n")));
  TEST_CHECK(str_eq(rewrite("/* one\n   two   */\n", 40), S("/* one two */\n")));
  TEST_CHECK(str_eq(rewrite("/* a\t\t\t\t\tb */\n", 20), S("/* a b */\n")));
  // The line, indent included, may fill the column exactly but not pass it.
  TEST_CHECK(str_eq(rewrite("/* one   two */\n", 15), S("/* one   two */\n")));
  TEST_CHECK(str_eq(rewrite("    /* one   two */\n", 18), S("    /* one two */\n")));
  // A line comment is still refilled, spacing included.
  TEST_CHECK(str_eq(rewrite("// one   two\n", 40), S("// one two\n")));
}

// A hanging closer-only last line is rewritten as a trailing closer on the last prose line.
static void test_hanging_closer_trails_last_prose(void) {
  const char* source = "  /* one two\n     three\n   */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(out, S("  /* one two three */\n")));
}

// A starred body that fits the opener line only without its closer wraps to keep the closer in
// width.
static void test_starred_opener_line_reserves_closer(void) {
  const char* source = "/* aaaa bbbb cccc dddd\n * eeee ffff gggg hhhh\n */\n";
  const char* expected = "/* aaaa bbbb cccc dddd eeee ffff gggg\n * hhhh\n */\n";
  const struct Str out = rewrite(source, 44);
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// A list-item continuation keeps the two-space hanging indent under the marker.
static void test_preserves_list_continuation_indent(void) {
  const char* source =
      "/**\n"
      " * - an unquoted attribute value (whitespace, `=` and a backtick pass through, so the value "
      "can end\n"
      " *   early and inject an attribute)\n"
      " */\n";
  const struct Str out = rewrite(source, 100);
  TEST_CHECK(str_eq(out, str_from_cstr(source)));
}

// A punctuation-only backtick span stays prose and travels with the word before it.
static void test_joins_leading_with_backtick_slash(void) {
  const char* source =
      "/**\n"
      " * The output-relative path is the returned URL without its leading\n"
      " * `/`.\n"
      " */\n";
  const struct Str out = rewrite(source, 100);
  const char* expected =
      "/**\n"
      " * The output-relative path is the returned URL without its leading `/`.\n"
      " */\n";
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// A hanging `*` list is not treated as starred decoration and is not joined into prose.
static void test_leaves_hanging_star_list(void) {
  const char* source = "/* Keep:\n   * first item\n   * second item\n */\n";
  const struct Str out = rewrite(source, 40);
  const char* expected = "/* Keep:\n   * first item\n   * second item */\n";
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

// An indented sample in a hanging block is not joined into one prose line.
static void test_leaves_hanging_indented_sample(void) {
  const char* source =
      "/* line 3:\n"
      "\n"
      "     1  int main(void) {\n"
      "     2    int x = 1;\n"
      "     3    return x;\n"
      "     4  }\n"
      " */\n";
  const struct Str out = rewrite(source, 100);
  const char* expected =
      "/* line 3:\n"
      "\n"
      "     1  int main(void) {\n"
      "     2    int x = 1;\n"
      "     3    return x;\n"
      "     4  } */\n";
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
}

/**
 * @brief Checks that rewriting `source` at `width` produces exactly `expected`.
 *
 * @param source   Terminated source text to rewrite.
 * @param width    Target display width in columns.
 * @param expected Terminated text the rewrite must produce.
 */
static void check_rewrite(const char* source, ptrdiff_t width, const char* expected) {
  const struct Str out = rewrite(source, width);
  TEST_CHECK(str_eq(out, str_from_cstr(expected)));
  TEST_MSG("output: '%.*s'", STR_FMT(out));
}

// A tool directive between prose lines stays on its own line and splits them into two paragraphs.
static void test_tool_directive_splits_paragraph(void) {
  check_rewrite("  // one\n  // two\n  // cppcheck-suppress deallocuse\n  // three\n  // four\n",
                40, "  // one two\n  // cppcheck-suppress deallocuse\n  // three four\n");
  check_rewrite("/*\n * one\n * two\n * NOLINTEND(a)\n * three\n */\n", 40,
                "/*\n * one two\n * NOLINTEND(a)\n * three\n */\n");
}

// A tool directive that opens or closes a run stays on its own line, and the prose beside it joins.
static void test_tool_directive_at_run_edges(void) {
  check_rewrite("// NOLINTNEXTLINE(misc-no-recursion)\n// one\n// two\n", 40,
                "// NOLINTNEXTLINE(misc-no-recursion)\n// one two\n");
  check_rewrite("// one\n// two\n// clang-format off\n", 40, "// one two\n// clang-format off\n");
}

// A tool directive wider than the column is copied unwrapped.
static void test_leaves_long_tool_directive_unwrapped(void) {
  const char* source = "  // NOLINT(bugprone-easily-swappable-parameters, misc-no-recursion)\n";
  check_rewrite(source, 30, source);
}

// A source with no terminator and no final newline is read up to its last byte and no further. The
// source is the head of a longer literal, so a read past its end finds `X` rather than a
// terminator, in any build.
static void test_rewrites_source_without_terminator(void) {
  const struct Str source = str_head(S("x;\n// one\n// twoXYZ"), S("x;\n// one\n// two").len);
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  rewrite_source(&out, source, 40, test_arena());
  TEST_CHECK(str_eq(buf_str(&out), S("x;\n// one two")));
}

// A rewrite whose scratch region cannot hold its comment spans ends in `os_oom` rather than
// overrunning the region. Under the test build's `os_oom` that is the process exiting with
// `TEST_OOM_EXIT_STATUS`.
static void test_tiny_scratch_ends_in_oom(void) {
  TEST_CHECK(test_is_oom(rewrite_in_tiny_scratch, NULL));
}

TEST_LIST = {
    {"leaves code and strings unchanged", test_leaves_code_and_strings_unchanged},
    {"leaves crlf spliced string unchanged", test_leaves_crlf_spliced_string_unchanged},
    {"leaves trailing comment unchanged", test_leaves_trailing_comment_unchanged},
    {"leaves spliced trailing comment unchanged", test_leaves_spliced_trailing_comment_unchanged},
    {"leaves comment after comment unchanged", test_leaves_comment_after_comment_unchanged},
    {"leaves comment opened in include path unchanged",
     test_leaves_comment_opened_in_include_path_unchanged},
    {"leaves spliced line comment unchanged", test_leaves_spliced_line_comment_unchanged},
    {"joins short line comments", test_joins_short_line_comments},
    {"narrow width splits atoms", test_narrow_width_splits_atoms},
    {"wraps wide emoji by display columns", test_wraps_wide_emoji_by_display_columns},
    {"wraps combining mark by display columns", test_wraps_combining_mark_by_display_columns},
    {"preserves crlf line endings", test_preserves_crlf_line_endings},
    {"preserves crlf in block shapes", test_preserves_crlf_in_block_shapes},
    {"counts inline doxygen opener width", test_counts_inline_doxygen_opener_width},
    {"tag in plain block stays prose", test_tag_in_plain_block_stays_prose},
    {"aligns direction parameters end to end", test_aligns_direction_parameters_end_to_end},
    {"tag name atom written once", test_tag_name_atom_written_once},
    {"reindents wrapped line comments", test_reindents_wrapped_line_comments},
    {"hanging blank line has no trailing spaces", test_hanging_blank_line_has_no_trailing_spaces},
    {"preserves hyphen after backtick", test_preserves_hyphen_after_backtick},
    {"preserves indent on hanging comment", test_preserves_indent_on_hanging_comment},
    {"keeps be on line when it fits", test_keeps_be_on_line_when_it_fits},
    {"breaks inside parenthetical", test_breaks_inside_parenthetical},
    {"one line block within width unchanged", test_one_line_block_within_width_unchanged},
    {"hanging closer trails last prose", test_hanging_closer_trails_last_prose},
    {"starred opener line reserves closer", test_starred_opener_line_reserves_closer},
    {"preserves list continuation indent", test_preserves_list_continuation_indent},
    {"joins leading with backtick slash", test_joins_leading_with_backtick_slash},
    {"leaves hanging star list", test_leaves_hanging_star_list},
    {"leaves hanging indented sample", test_leaves_hanging_indented_sample},
    {"tool directive splits paragraph", test_tool_directive_splits_paragraph},
    {"tool directive at run edges", test_tool_directive_at_run_edges},
    {"leaves long tool directive unwrapped", test_leaves_long_tool_directive_unwrapped},
    {"rewrites source without terminator", test_rewrites_source_without_terminator},
    {"tiny scratch ends in oom", test_tiny_scratch_ends_in_oom},
    {NULL, NULL},
};
