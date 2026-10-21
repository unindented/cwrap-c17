#include <acutest.h>
#include <stddef.h>
#include <stdio.h>

#include "domain/comment.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Groups source text into wrappable comment blocks.
 *
 * @param source Terminated source text to group.
 * @param arena  Arena that owns the returned list storage. Must not be `NULL`.
 * @return The grouped comment-block list.
 */
static struct CommentBlockList group(const char* source, struct Arena* arena) {
  struct CommentSpanList spans = lex_comment_spans(str_from_cstr(source), arena);
  return comment_group(str_from_cstr(source), &spans, arena);
}

/**
 * @brief Groups `source` and extracts the body lines of its only block.
 *
 * @param source Terminated source text holding one wrappable block.
 * @param arena  Arena that owns the grouped blocks and extracted lines. Must not be `NULL`.
 * @return The extracted body-line list.
 */
static struct BodyLineList extract(const char* source, struct Arena* arena) {
  struct CommentBlockList blocks = group(source, arena);
  TEST_ASSERT(blocks.count == 1);
  return comment_extract_body(str_from_cstr(source), &blocks.items[0], arena);
}

// Consecutive same-indent line comments become one block. A trailing comment is omitted.
static void test_groups_line_runs_and_skips_trailing(void) {
  struct Arena arena = test_arena();
  const char* source = "// one\n// two\nint x; // skip\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_CHECK(blocks.count == 1);
  TEST_CHECK(blocks.items[0].shape == COMMENT_SHAPE_LINE);
  TEST_CHECK(blocks.items[0].end > blocks.items[0].start);
}

// A spliced line comment is omitted and ends the line run before it.
static void test_groups_skip_spliced_line_comment(void) {
  struct Arena arena = test_arena();
  const char* source = "// one\n// two \\\n  three\n// four\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_CHECK(blocks.count == 2);
  TEST_CHECK(blocks.items[0].start == 0);
  TEST_CHECK(blocks.items[0].end == S("// one").len);
  TEST_CHECK(blocks.items[1].start == S("// one\n// two \\\n  three\n").len);
}

// Each opener is recorded exactly, and only the four Doxygen markers enable commands.
static void test_records_opener_marker(void) {
  struct Arena arena = test_arena();
  const char* sources[] = {"// a\n", "/// a\n", "//! a\n", "/* a */\n", "/** a */\n", "/*! a */\n"};
  const char* openers[] = {"//", "///", "//!", "/*", "/**", "/*!"};
  const bool is_doxygen[] = {false, true, true, false, true, true};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(sources) / sizeof(sources[0])); i++) {
    struct CommentBlockList blocks = group(sources[i], &arena);
    TEST_ASSERT(blocks.count == 1);
    TEST_CHECK(str_eq(blocks.items[0].opener, str_from_cstr(openers[i])));
    TEST_CHECK(blocks.items[0].is_doxygen == is_doxygen[i]);
    TEST_MSG("source: '%s'", sources[i]);
  }
}

// A line run groups only lines that share a marker. Four slashes are a plain `//` comment.
static void test_groups_line_runs_by_marker(void) {
  struct Arena arena = test_arena();
  const char* source = "/// a\n/// b\n// c\n//// d\n//! e\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 3);
  TEST_CHECK(str_eq(blocks.items[0].opener, S("///")));
  TEST_CHECK(blocks.items[0].end == S("/// a\n/// b").len);
  TEST_CHECK(str_eq(blocks.items[1].opener, S("//")));
  TEST_CHECK(blocks.items[1].end == S("/// a\n/// b\n// c\n//// d").len);
  TEST_CHECK(str_eq(blocks.items[2].opener, S("//!")));
}

// An indented line run groups like a top-level one, and its block keeps the shared indent.
static void test_groups_indented_line_run(void) {
  struct Arena arena = test_arena();
  const char* source = "{\n  // one\n  // two\n  // three\n}\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 1);
  TEST_CHECK(blocks.items[0].start == S("{\n  ").len);
  TEST_CHECK(blocks.items[0].end == S("{\n  // one\n  // two\n  // three").len);
  TEST_CHECK(blocks.items[0].indent_columns == 2);
}

// A line at a different indent starts a new block, whether it is deeper or shallower.
static void test_groups_split_line_run_at_indent_change(void) {
  struct Arena arena = test_arena();
  const char* source = "  // one\n    // two\n  // three\n// four\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 4);
  TEST_CHECK(blocks.items[0].end == S("  // one").len);
  TEST_CHECK(blocks.items[1].start == S("  // one\n    ").len);
  TEST_CHECK(blocks.items[1].end == S("  // one\n    // two").len);
  TEST_CHECK(blocks.items[2].indent_columns == 2);
  TEST_CHECK(blocks.items[3].indent_columns == 0);
}

// A tab indent is measured in display columns, so it matches eight spaces and no fewer.
static void test_groups_tab_indented_line_run(void) {
  struct Arena arena = test_arena();
  const char* source = "\t// one\n        // two\n\t// three\n  // four\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 2);
  TEST_CHECK(blocks.items[0].end == S("\t// one\n        // two\n\t// three").len);
  TEST_CHECK(blocks.items[0].indent_columns == 8);
  TEST_CHECK(blocks.items[1].indent_columns == 2);
}

// An indented line run groups across CRLF line breaks and records the CRLF convention.
static void test_groups_crlf_indented_line_run(void) {
  struct Arena arena = test_arena();
  const char* source = "  // one\r\n  // two\r\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 1);
  TEST_CHECK(blocks.items[0].end == S("  // one\r\n  // two").len);
  TEST_CHECK(blocks.items[0].has_crlf_newlines);
}

// A code line or a blank line between two indented line comments ends the run.
static void test_groups_split_line_run_at_code_or_blank_line(void) {
  struct Arena arena = test_arena();
  const char* source = "  // one\n  x++;\n  // two\n\n  // three\n  \n  // four\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 4);
  TEST_CHECK(blocks.items[0].end == S("  // one").len);
  TEST_CHECK(blocks.items[1].end == S("  // one\n  x++;\n  // two").len);
  TEST_CHECK(blocks.items[2].end == S("  // one\n  x++;\n  // two\n\n  // three").len);
}

// A block with no letter, digit, or non-ASCII byte is omitted, so a banner is copied unchanged.
static void test_groups_skip_blocks_without_prose(void) {
  struct Arena arena = test_arena();
  const char* source = "/**/\n/*****************/\n// ----\n// ====\n/* é */\n";
  struct CommentBlockList blocks = group(source, &arena);
  TEST_ASSERT(blocks.count == 1);
  TEST_CHECK(blocks.items[0].start == S("/**/\n/*****************/\n// ----\n// ====\n").len);
}

// Continuation stars classify a block as starred; their absence classifies it as hanging.
static void test_classifies_starred_and_hanging(void) {
  struct Arena arena = test_arena();
  struct CommentBlockList starred = group("/**\n * foo\n */\n", &arena);
  TEST_CHECK(starred.count == 1);
  TEST_CHECK(starred.items[0].shape == COMMENT_SHAPE_STARRED);
  TEST_CHECK(starred.items[0].is_doxygen);
  TEST_CHECK(str_eq(starred.items[0].opener, S("/**")));
  TEST_CHECK(starred.items[0].is_opener_empty);

  struct CommentBlockList hanging = group("/* foo\n   bar */\n", &arena);
  TEST_CHECK(hanging.count == 1);
  TEST_CHECK(hanging.items[0].shape == COMMENT_SHAPE_HANGING);

  struct CommentBlockList star_list = group("/* Keep:\n   * first\n   * second\n */\n", &arena);
  TEST_CHECK(star_list.count == 1);
  TEST_CHECK(star_list.items[0].shape == COMMENT_SHAPE_HANGING);
}

// A closer-only last line is dropped for hanging blocks as well as starred ones.
static void test_extract_drops_hanging_closer_line(void) {
  struct Arena arena = test_arena();
  const char* source = "/* foo\n   bar\n */\n";
  struct CommentBlockList blocks = group(source, &arena);
  struct BodyLineList lines = comment_extract_body(str_from_cstr(source), &blocks.items[0], &arena);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(str_eq(lines.items[0].text, S("foo")));
  TEST_CHECK(str_eq(lines.items[1].text, S("bar")));
}

// Extra indent past the hanging column stays on the payload and is classified as a sample.
static void test_extract_keeps_hanging_sample_indent(void) {
  struct Arena arena = test_arena();
  const char* source = "/* line 3:\n\n     1  int x;\n     2  x = 0;\n */\n";
  struct CommentBlockList blocks = group(source, &arena);
  struct BodyLineList lines = comment_extract_body(str_from_cstr(source), &blocks.items[0], &arena);
  TEST_CHECK(lines.count == 4);
  TEST_CHECK(lines.items[2].kind == BODY_LINE_CODE);
  TEST_CHECK(str_eq(lines.items[2].text, S("  1  int x;")));
  TEST_CHECK(lines.items[3].kind == BODY_LINE_CODE);
  TEST_CHECK(str_eq(lines.items[3].text, S("  2  x = 0;")));
}

// Extracting a starred block drops the closer-only last line.
static void test_extract_drops_starred_closer_line(void) {
  struct Arena arena = test_arena();
  const char* source = "/**\n * foo\n */\n";
  struct CommentBlockList blocks = group(source, &arena);
  struct BodyLineList lines = comment_extract_body(str_from_cstr(source), &blocks.items[0], &arena);
  TEST_CHECK(lines.count == 1);
  TEST_CHECK(str_eq(lines.items[0].text, S("foo")));
}

// Each known tool directive, with or without a suffix, is a directive line in any comment shape.
static void test_extract_classifies_tool_directives(void) {
  struct Arena arena = test_arena();
  const char* payloads[] = {
      "cppcheck-suppress deallocuse",
      "cppcheck-suppress-file unusedFunction",
      "NOLINT",
      "NOLINT(bugprone-branch-clone)",
      "NOLINTNEXTLINE",
      "NOLINTNEXTLINE(misc-no-recursion)",
      "NOLINTBEGIN(cert-err33-c)",
      "NOLINTEND",
      "clang-format off",
      "clang-format on",
      "IWYU pragma: keep",
      "IWYU pragma:export",
  };
  const char* openers[] = {"// ", "/* ", "/**\n * "};
  const char* closers[] = {"\n", " */\n", "\n */\n"};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(payloads) / sizeof(payloads[0])); i++) {
    for (ptrdiff_t j = 0; j < (ptrdiff_t)(sizeof(openers) / sizeof(openers[0])); j++) {
      char source[128];
      TEST_ASSERT(snprintf(source, sizeof(source), "%s%s%s", openers[j], payloads[i], closers[j]) >
                  0);
      struct BodyLineList lines = extract(source, &arena);
      TEST_ASSERT(lines.count == 1);
      TEST_CHECK(lines.items[0].kind == BODY_LINE_DIRECTIVE);
      TEST_CHECK(str_eq(lines.items[0].text, str_from_cstr(payloads[i])));
      TEST_MSG("source: '%s'", source);
    }
  }
}

// A directive matches only as a whole word, so a longer word that starts with one is prose.
static void test_extract_tool_directive_needs_word_boundary(void) {
  struct Arena arena = test_arena();
  const char* sources[] = {
      "// NOLINTED\n",
      "// NOLINT_SOON\n",
      "// cppcheck-suppressed\n",
      "// clang-format offset\n",
      "// IWYU pragma keep\n",
      "// nolint\n",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(sources) / sizeof(sources[0])); i++) {
    struct BodyLineList lines = extract(sources[i], &arena);
    TEST_ASSERT(lines.count == 1);
    TEST_CHECK(lines.items[0].kind == BODY_LINE_PROSE);
    TEST_MSG("source: '%s'", sources[i]);
  }
}

// A prose line indented at any depth after a tag continues it and loses its indent.
static void test_extract_tag_continuation_at_any_depth(void) {
  struct Arena arena = test_arena();
  const char* source = "/**\n * @brief One\n *   two\n *             three\n *\n *   sample\n */\n";
  struct BodyLineList lines = extract(source, &arena);
  TEST_ASSERT(lines.count == 5);
  TEST_CHECK(lines.items[0].kind == BODY_LINE_TAG);
  TEST_CHECK(lines.items[1].kind == BODY_LINE_PROSE);
  TEST_CHECK(str_eq(lines.items[1].text, S("two")));
  TEST_CHECK(lines.items[2].kind == BODY_LINE_PROSE);
  TEST_CHECK(str_eq(lines.items[2].text, S("three")));
  TEST_CHECK(lines.items[4].kind == BODY_LINE_CODE);
  TEST_CHECK(str_eq(lines.items[4].text, S("  sample")));
}

// A list item or decoration indented under a tag keeps its indent and ends the tag.
static void test_extract_indented_list_ends_tag(void) {
  struct Arena arena = test_arena();
  const char* source = "/**\n * @param mode One of:\n *   - first\n *   more\n */\n";
  struct BodyLineList lines = extract(source, &arena);
  TEST_ASSERT(lines.count == 3);
  TEST_CHECK(lines.items[1].kind == BODY_LINE_CODE);
  TEST_CHECK(str_eq(lines.items[1].text, S("  - first")));
  TEST_CHECK(lines.items[2].kind == BODY_LINE_CODE);
}

// A Doxygen code or verbatim region, fence lines included, is a sample in either spelling.
static void test_extract_doxygen_fences(void) {
  struct Arena arena = test_arena();
  const char* source =
      "/**\n * @param x Example:\n *        @code{.c}\n * @brief f(x);\n *        @endcode\n"
      " * \\verbatim\n * @endcode\n * \\endverbatim\n * After.\n */\n";
  struct BodyLineList lines = extract(source, &arena);
  TEST_ASSERT(lines.count == 8);
  TEST_CHECK(lines.items[0].kind == BODY_LINE_TAG);
  for (ptrdiff_t i = 1; i < 7; i++) {
    TEST_CHECK(lines.items[i].kind == BODY_LINE_CODE);
    TEST_MSG("line %td: '%.*s'", i, STR_FMT(lines.items[i].text));
  }
  TEST_CHECK(lines.items[7].kind == BODY_LINE_PROSE);
}

// Commands are prose in a plain `//` or slash-star block, and a tag in a Doxygen line block.
static void test_extract_commands_only_in_doxygen_blocks(void) {
  struct Arena arena = test_arena();
  const char* sources[] = {"// @brief a\n", "/* @brief a */\n", "/// @brief a\n", "//! @brief a\n"};
  const enum BodyLineKind kinds[] = {BODY_LINE_PROSE, BODY_LINE_PROSE, BODY_LINE_TAG,
                                     BODY_LINE_TAG};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(sources) / sizeof(sources[0])); i++) {
    struct BodyLineList lines = extract(sources[i], &arena);
    TEST_ASSERT(lines.count == 1);
    TEST_CHECK(lines.items[0].kind == kinds[i]);
    TEST_CHECK(str_eq(lines.items[0].text, S("@brief a")));
    TEST_MSG("source: '%s'", sources[i]);
  }
}

// An extracted line is a view into the source rather than a copy, so it needs no terminator.
static void test_extract_lines_are_views_into_source(void) {
  struct Arena arena = test_arena();
  const char* source = "/* one\n   two */\n";
  struct BodyLineList lines = extract(source, &arena);
  TEST_ASSERT(lines.count == 2);
  TEST_CHECK(str_eq(lines.items[0].text, S("one")));
  TEST_CHECK(lines.items[0].text.data == source + S("/* ").len);
  TEST_CHECK(str_eq(lines.items[1].text, S("two")));
  TEST_CHECK(lines.items[1].text.data == source + S("/* one\n   ").len);
}

// Emit restores a hanging closer on the last prose line.
static void test_emit_hanging_trails_closer(void) {
  struct Arena arena = test_arena();
  const char* source = "/* foo */\n";
  struct CommentBlockList blocks = group(source, &arena);
  struct BodyLineList lines = comment_extract_body(str_from_cstr(source), &blocks.items[0], &arena);
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  comment_emit(&out, str_from_cstr(source), &blocks.items[0], &lines);
  TEST_CHECK(str_eq(buf_str(&out), S("/* foo */")));
}

// Emit writes each line of a line block back with the block's own marker.
static void test_emit_restores_line_marker(void) {
  struct Arena arena = test_arena();
  const char* source = "//! one\n//!\n//! two\n";
  struct BodyLineList lines = extract(source, &arena);
  struct CommentBlockList blocks = group(source, &arena);
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  comment_emit(&out, str_from_cstr(source), &blocks.items[0], &lines);
  TEST_CHECK(str_eq(buf_str(&out), S("//! one\n//!\n//! two")));
}

// Prefix widths count the recorded marker plus one space, except a starred continuation, which is
// ` * `.
static void test_prefix_columns_follow_marker(void) {
  struct Arena arena = test_arena();
  struct CommentBlockList line = group("  /// a\n", &arena);
  TEST_CHECK(comment_prefix_columns(&line.items[0]) == 6);
  TEST_CHECK(comment_prefix_columns_first(&line.items[0]) == 6);
  struct CommentBlockList bang = group("/*! a\n * b\n */\n", &arena);
  TEST_CHECK(comment_prefix_columns(&bang.items[0]) == 3);
  TEST_CHECK(comment_prefix_columns_first(&bang.items[0]) == 4);
  struct CommentBlockList hanging = group("/** a\n    b */\n", &arena);
  TEST_CHECK(comment_prefix_columns(&hanging.items[0]) == 4);
  TEST_CHECK(comment_prefix_columns_first(&hanging.items[0]) == 4);
}

// Only a starred block with one body line on the opener puts its closer on that line.
static void test_has_opener_closer_for_one_line_starred(void) {
  struct Arena arena = test_arena();
  const char* starred_source = "/* foo\n * bar\n */\n";
  struct CommentBlockList starred = group(starred_source, &arena);
  struct BodyLineList lines =
      comment_extract_body(str_from_cstr(starred_source), &starred.items[0], &arena);
  TEST_CHECK(!comment_has_opener_closer(&starred.items[0], &lines));
  lines.count = 1;
  TEST_CHECK(comment_has_opener_closer(&starred.items[0], &lines));

  const char* hanging_source = "/* foo */\n";
  struct CommentBlockList hanging = group(hanging_source, &arena);
  lines = comment_extract_body(str_from_cstr(hanging_source), &hanging.items[0], &arena);
  TEST_CHECK(!comment_has_opener_closer(&hanging.items[0], &lines));
}

// Emit starts at the opener. The original indent lives in the source gap before the block.
static void test_emit_omits_first_line_indent(void) {
  struct Arena arena = test_arena();
  const char* source = "  /* foo */\n";
  struct CommentBlockList blocks = group(source, &arena);
  struct BodyLineList lines = comment_extract_body(str_from_cstr(source), &blocks.items[0], &arena);
  struct Arena out_arena = test_arena();
  struct Buf out = {.arena = &out_arena};
  comment_emit(&out, str_from_cstr(source), &blocks.items[0], &lines);
  TEST_CHECK(str_eq(buf_str(&out), S("/* foo */")));
}

// A list marker or a tool directive is read only from the bytes of its view, as when a refill tests
// the rest of a paragraph from one of its words.
static void test_line_classifiers_stop_at_view_end(void) {
  TEST_CHECK(comment_is_list_line(S("- a")));
  TEST_CHECK(!comment_is_list_line(str_head(S("- a"), 1)));
  TEST_CHECK(comment_is_list_line(S("12. a")));
  TEST_CHECK(!comment_is_list_line(str_head(S("12. a"), 3)));
  TEST_CHECK(!comment_is_list_line(S("12")));
  TEST_CHECK(comment_is_tool_directive(str_head(S("NOLINTED"), 6)));
  TEST_CHECK(!comment_is_tool_directive(S("NOLINTED")));
  TEST_CHECK(!comment_is_tool_directive(str_head(S("NOLINT"), 5)));
}

TEST_LIST = {
    {"groups line runs and skips trailing", test_groups_line_runs_and_skips_trailing},
    {"groups skip spliced line comment", test_groups_skip_spliced_line_comment},
    {"records opener marker", test_records_opener_marker},
    {"groups line runs by marker", test_groups_line_runs_by_marker},
    {"groups indented line run", test_groups_indented_line_run},
    {"groups split line run at indent change", test_groups_split_line_run_at_indent_change},
    {"groups tab indented line run", test_groups_tab_indented_line_run},
    {"groups crlf indented line run", test_groups_crlf_indented_line_run},
    {"groups split line run at code or blank line",
     test_groups_split_line_run_at_code_or_blank_line},
    {"groups skip blocks without prose", test_groups_skip_blocks_without_prose},
    {"classifies starred and hanging", test_classifies_starred_and_hanging},
    {"extract drops hanging closer line", test_extract_drops_hanging_closer_line},
    {"extract keeps hanging sample indent", test_extract_keeps_hanging_sample_indent},
    {"extract drops starred closer line", test_extract_drops_starred_closer_line},
    {"extract classifies tool directives", test_extract_classifies_tool_directives},
    {"extract tool directive needs word boundary", test_extract_tool_directive_needs_word_boundary},
    {"extract tag continuation at any depth", test_extract_tag_continuation_at_any_depth},
    {"extract indented list ends tag", test_extract_indented_list_ends_tag},
    {"extract doxygen fences", test_extract_doxygen_fences},
    {"extract commands only in doxygen blocks", test_extract_commands_only_in_doxygen_blocks},
    {"extract lines are views into source", test_extract_lines_are_views_into_source},
    {"emit hanging trails closer", test_emit_hanging_trails_closer},
    {"emit restores line marker", test_emit_restores_line_marker},
    {"prefix columns follow marker", test_prefix_columns_follow_marker},
    {"has opener closer for one line starred", test_has_opener_closer_for_one_line_starred},
    {"emit omits first line indent", test_emit_omits_first_line_indent},
    {"line classifiers stop at view end", test_line_classifiers_stop_at_view_end},
    {NULL, NULL},
};
