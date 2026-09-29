#include <acutest.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "domain/comment.h"
#include "domain/lex.h"
#include "shared/arena.h"
#include "shared/string_buffer.h"

/**
 * @brief Groups source text into wrapable comment blocks.
 *
 * @param arena  Arena that owns the returned list storage.
 * @param source Terminated source text to group.
 * @return The grouped comment-block list.
 */
static struct CommentBlockList group(struct Arena* arena, const char* source) {
  struct CommentSpanList spans;
  char err[64];
  TEST_CHECK(lex_comment_spans(source, strlen(source), arena, &spans, err, sizeof(err)) == 0);
  struct CommentBlockList blocks;
  TEST_CHECK(comment_group(source, strlen(source), &spans, arena, &blocks, err, sizeof(err)) == 0);
  return blocks;
}

/**
 * @brief Groups `source` and extracts the body lines of its only block.
 *
 * @param arena  Arena that owns the grouped blocks and extracted lines.
 * @param source Terminated source text holding one wrapable block.
 * @return The extracted body-line list.
 */
static struct BodyLineList extract(struct Arena* arena, const char* source) {
  struct CommentBlockList blocks = group(arena, source);
  TEST_ASSERT(blocks.count == 1);
  struct BodyLineList lines = {0};
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], arena, &lines, err, sizeof(err)) == 0);
  return lines;
}

// Consecutive same-indent line comments become one block. A trailing comment is omitted.
static void test_groups_line_runs_and_skips_trailing(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "// one\n// two\nint x; // skip\n";
  struct CommentBlockList blocks = group(&arena, source);
  TEST_CHECK(blocks.count == 1);
  TEST_CHECK(blocks.items[0].shape == COMMENT_SHAPE_LINE);
  TEST_CHECK(blocks.items[0].end > blocks.items[0].start);
  arena_free(&arena);
}

// A spliced line comment is omitted and ends the line run before it.
static void test_groups_skip_spliced_line_comment(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "// one\n// two \\\n  three\n// four\n";
  struct CommentBlockList blocks = group(&arena, source);
  TEST_CHECK(blocks.count == 2);
  TEST_CHECK(blocks.items[0].start == 0);
  TEST_CHECK(blocks.items[0].end == strlen("// one"));
  TEST_CHECK(blocks.items[1].start == strlen("// one\n// two \\\n  three\n"));
  arena_free(&arena);
}

// Each opener is recorded exactly, and only the four Doxygen markers enable commands.
static void test_records_opener_marker(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* sources[] = {"// a\n", "/// a\n", "//! a\n", "/* a */\n", "/** a */\n", "/*! a */\n"};
  const char* openers[] = {"//", "///", "//!", "/*", "/**", "/*!"};
  const bool is_doxygen[] = {false, true, true, false, true, true};
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
    struct CommentBlockList blocks = group(&arena, sources[i]);
    TEST_ASSERT(blocks.count == 1);
    TEST_CHECK(strcmp(blocks.items[0].opener, openers[i]) == 0);
    TEST_CHECK(blocks.items[0].is_doxygen == is_doxygen[i]);
    TEST_MSG("source: '%s'", sources[i]);
  }
  arena_free(&arena);
}

// A line run groups only lines that share a marker. Four slashes are a plain `//` comment.
static void test_groups_line_runs_by_marker(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/// a\n/// b\n// c\n//// d\n//! e\n";
  struct CommentBlockList blocks = group(&arena, source);
  TEST_ASSERT(blocks.count == 3);
  TEST_CHECK(strcmp(blocks.items[0].opener, "///") == 0);
  TEST_CHECK(blocks.items[0].end == strlen("/// a\n/// b"));
  TEST_CHECK(strcmp(blocks.items[1].opener, "//") == 0);
  TEST_CHECK(blocks.items[1].end == strlen("/// a\n/// b\n// c\n//// d"));
  TEST_CHECK(strcmp(blocks.items[2].opener, "//!") == 0);
  arena_free(&arena);
}

// A block with no letter, digit, or non-ASCII byte is omitted, so a banner is copied unchanged.
static void test_groups_skip_blocks_without_prose(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/**/\n/*****************/\n// ----\n// ====\n/* é */\n";
  struct CommentBlockList blocks = group(&arena, source);
  TEST_ASSERT(blocks.count == 1);
  TEST_CHECK(blocks.items[0].start == strlen("/**/\n/*****************/\n// ----\n// ====\n"));
  arena_free(&arena);
}

// Continuation stars classify a block as starred; their absence classifies it as hanging.
static void test_classifies_starred_and_hanging(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentBlockList starred = group(&arena, "/**\n * foo\n */\n");
  TEST_CHECK(starred.count == 1);
  TEST_CHECK(starred.items[0].shape == COMMENT_SHAPE_STARRED);
  TEST_CHECK(starred.items[0].is_doxygen);
  TEST_CHECK(strcmp(starred.items[0].opener, "/**") == 0);
  TEST_CHECK(starred.items[0].is_opener_empty);

  struct CommentBlockList hanging = group(&arena, "/* foo\n   bar */\n");
  TEST_CHECK(hanging.count == 1);
  TEST_CHECK(hanging.items[0].shape == COMMENT_SHAPE_HANGING);

  struct CommentBlockList star_list = group(&arena, "/* Keep:\n   * first\n   * second\n */\n");
  TEST_CHECK(star_list.count == 1);
  TEST_CHECK(star_list.items[0].shape == COMMENT_SHAPE_HANGING);
  arena_free(&arena);
}

// A closer-only last line is dropped for hanging blocks as well as starred ones.
static void test_extract_drops_hanging_closer_line(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/* foo\n   bar\n */\n";
  struct CommentBlockList blocks = group(&arena, source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], &arena, &lines, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(strcmp(lines.items[0].text, "foo") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "bar") == 0);
  arena_free(&arena);
}

// Extra indent past the hanging column stays on the payload and is classified as a sample.
static void test_extract_keeps_hanging_sample_indent(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/* line 3:\n\n     1  +++\n     2  title\n */\n";
  struct CommentBlockList blocks = group(&arena, source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], &arena, &lines, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 4);
  TEST_CHECK(lines.items[2].kind == BODY_LINE_CODE);
  TEST_CHECK(strcmp(lines.items[2].text, "  1  +++") == 0);
  TEST_CHECK(lines.items[3].kind == BODY_LINE_CODE);
  TEST_CHECK(strcmp(lines.items[3].text, "  2  title") == 0);
  arena_free(&arena);
}

// Extracting a starred block drops the closer-only last line.
static void test_extract_drops_starred_closer_line(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/**\n * foo\n */\n";
  struct CommentBlockList blocks = group(&arena, source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], &arena, &lines, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 1);
  TEST_CHECK(strcmp(lines.items[0].text, "foo") == 0);
  arena_free(&arena);
}

// Emit restores a hanging closer on the last prose line.
static void test_emit_hanging_trails_closer(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "/* foo */\n";
  struct CommentBlockList blocks = group(&arena, source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], &arena, &lines, err, sizeof(err)) == 0);
  struct StringBuffer out;
  string_buffer_init(&out);
  TEST_CHECK(comment_emit(&out, source, &blocks.items[0], &lines) == 0);
  TEST_CHECK(out.data != NULL && strcmp(out.data, "/* foo */") == 0);
  string_buffer_free(&out);
  arena_free(&arena);
}

// Emit writes each line of a line block back with the block's own marker.
static void test_emit_restores_line_marker(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "//! one\n//!\n//! two\n";
  struct BodyLineList lines = extract(&arena, source);
  struct CommentBlockList blocks = group(&arena, source);
  struct StringBuffer out;
  string_buffer_init(&out);
  TEST_CHECK(comment_emit(&out, source, &blocks.items[0], &lines) == 0);
  TEST_CHECK(out.data != NULL && strcmp(out.data, "//! one\n//!\n//! two") == 0);
  string_buffer_free(&out);
  arena_free(&arena);
}

// Prefix widths count the recorded marker plus one space.
static void test_prefix_columns_follow_marker(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentBlockList line = group(&arena, "  /// a\n");
  TEST_CHECK(comment_prefix_columns(&line.items[0]) == 6);
  TEST_CHECK(comment_first_prefix_columns(&line.items[0]) == 6);
  struct CommentBlockList bang = group(&arena, "/*! a\n * b\n */\n");
  TEST_CHECK(comment_prefix_columns(&bang.items[0]) == 3);
  TEST_CHECK(comment_first_prefix_columns(&bang.items[0]) == 4);
  arena_free(&arena);
}

// Only a starred block with one body line on the opener puts its closer on that line.
static void test_has_opener_closer_for_one_line_starred(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* starred_source = "/* foo\n * bar\n */\n";
  struct CommentBlockList starred = group(&arena, starred_source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(starred_source, &starred.items[0], &arena, &lines, err,
                                  sizeof(err)) == 0);
  TEST_CHECK(!comment_has_opener_closer(&starred.items[0], &lines));
  lines.count = 1;
  TEST_CHECK(comment_has_opener_closer(&starred.items[0], &lines));

  const char* hanging_source = "/* foo */\n";
  struct CommentBlockList hanging = group(&arena, hanging_source);
  TEST_CHECK(comment_extract_body(hanging_source, &hanging.items[0], &arena, &lines, err,
                                  sizeof(err)) == 0);
  TEST_CHECK(!comment_has_opener_closer(&hanging.items[0], &lines));
  arena_free(&arena);
}

// Emit starts at the opener. The original indent lives in the source gap before the block.
static void test_emit_omits_first_line_indent(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* source = "  /* foo */\n";
  struct CommentBlockList blocks = group(&arena, source);
  struct BodyLineList lines;
  char err[64];
  TEST_CHECK(comment_extract_body(source, &blocks.items[0], &arena, &lines, err, sizeof(err)) == 0);
  struct StringBuffer out;
  string_buffer_init(&out);
  TEST_CHECK(comment_emit(&out, source, &blocks.items[0], &lines) == 0);
  TEST_CHECK(out.data != NULL && strcmp(out.data, "/* foo */") == 0);
  string_buffer_free(&out);
  arena_free(&arena);
}

TEST_LIST = {
    {"groups line runs and skips trailing", test_groups_line_runs_and_skips_trailing},
    {"groups skip spliced line comment", test_groups_skip_spliced_line_comment},
    {"records opener marker", test_records_opener_marker},
    {"groups line runs by marker", test_groups_line_runs_by_marker},
    {"groups skip blocks without prose", test_groups_skip_blocks_without_prose},
    {"classifies starred and hanging", test_classifies_starred_and_hanging},
    {"extract drops hanging closer line", test_extract_drops_hanging_closer_line},
    {"extract keeps hanging sample indent", test_extract_keeps_hanging_sample_indent},
    {"extract drops starred closer line", test_extract_drops_starred_closer_line},
    {"emit hanging trails closer", test_emit_hanging_trails_closer},
    {"emit restores line marker", test_emit_restores_line_marker},
    {"prefix columns follow marker", test_prefix_columns_follow_marker},
    {"has opener closer for one line starred", test_has_opener_closer_for_one_line_starred},
    {"emit omits first line indent", test_emit_omits_first_line_indent},
    {NULL, NULL},
};
