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

// Continuation stars classify a block as starred; their absence classifies it as hanging.
static void test_classifies_starred_and_hanging(void) {
  struct Arena arena;
  arena_init(&arena);
  struct CommentBlockList starred = group(&arena, "/**\n * foo\n */\n");
  TEST_CHECK(starred.count == 1);
  TEST_CHECK(starred.items[0].shape == COMMENT_SHAPE_STARRED);
  TEST_CHECK(starred.items[0].is_doxygen_opener);
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
    {"classifies starred and hanging", test_classifies_starred_and_hanging},
    {"extract drops hanging closer line", test_extract_drops_hanging_closer_line},
    {"extract keeps hanging sample indent", test_extract_keeps_hanging_sample_indent},
    {"extract drops starred closer line", test_extract_drops_starred_closer_line},
    {"emit hanging trails closer", test_emit_hanging_trails_closer},
    {"emit omits first line indent", test_emit_omits_first_line_indent},
    {NULL, NULL},
};
