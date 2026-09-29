#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "domain/comment.h"
#include "domain/fill.h"
#include "shared/arena.h"

// A short pair of prose lines is joined when both fit on one line.
static void test_joins_short_prose_lines(void) {
  struct Arena arena;
  arena_init(&arena);
  char one[] = "one";
  char two[] = "two";
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, one, 3},
      {BODY_LINE_PROSE, two, 3},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 20, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 1);
  TEST_CHECK(strcmp(lines.items[0].text, "one two") == 0);
  arena_free(&arena);
}

// A list item is left untouched even when the line is short.
static void test_leaves_list_items_untouched(void) {
  struct Arena arena;
  arena_init(&arena);
  char item[] = "- first";
  char prose[] = "second";
  struct BodyLine items[] = {
      {BODY_LINE_LIST, item, 7},
      {BODY_LINE_PROSE, prose, 6},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 40, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(strcmp(lines.items[0].text, "- first") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "second") == 0);
  arena_free(&arena);
}

// A wrapped tag description continues at the description column.
static void test_indents_wrapped_tag_description(void) {
  struct Arena arena;
  arena_init(&arena);
  char tag[] = "@param longername short more of the last tag";
  struct BodyLine items[] = {
      {BODY_LINE_TAG, tag, strlen(tag)},
  };
  struct BodyLineList lines = {items, 1};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 40, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(strcmp(lines.items[0].text, "@param longername short more of the") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "                  last tag") == 0);
  arena_free(&arena);
}

// A tag's continuation joins it, and the refilled description hangs under its first word.
static void test_refills_tag_continuation(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_TAG, "@note one two", strlen("@note one two")},
      {BODY_LINE_PROSE, "three four five", strlen("three four five")},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 24, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(lines.items[0].kind == BODY_LINE_TAG);
  TEST_CHECK(strcmp(lines.items[0].text, "@note one two three") == 0);
  TEST_CHECK(lines.items[1].kind == BODY_LINE_PROSE);
  TEST_CHECK(strcmp(lines.items[1].text, "      four five") == 0);
  arena_free(&arena);
}

// A tag line starts its own paragraph and is not joined with the prose above.
static void test_tag_line_starts_new_paragraph(void) {
  struct Arena arena;
  arena_init(&arena);
  char prose[] = "Description.";
  char tag[] = "@param x value";
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, prose, 12},
      {BODY_LINE_TAG, tag, 14},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 40, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(strcmp(lines.items[0].text, "Description.") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "@param x value") == 0);
  arena_free(&arena);
}

// Multiple paragraphs are rebuilt once while blank and list lines retain their order.
static void test_rebuilds_multiple_paragraphs_in_one_pass(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, "one", 3},   {BODY_LINE_PROSE, "two", 3},  {BODY_LINE_BLANK, "", 0},
      {BODY_LINE_PROSE, "three", 5}, {BODY_LINE_PROSE, "four", 4}, {BODY_LINE_BLANK, "", 0},
      {BODY_LINE_LIST, "- item", 6},
  };
  struct BodyLineList lines = {items, 7};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 40, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 5);
  TEST_CHECK(strcmp(lines.items[0].text, "one two") == 0);
  TEST_CHECK(lines.items[1].kind == BODY_LINE_BLANK);
  TEST_CHECK(strcmp(lines.items[2].text, "three four") == 0);
  TEST_CHECK(lines.items[3].kind == BODY_LINE_BLANK);
  TEST_CHECK(strcmp(lines.items[4].text, "- item") == 0);
  arena_free(&arena);
}

// A command word or list marker travels with the word before it, so no continuation line starts
// with one and reads back as a tag or list item.
static void test_keeps_line_openers_off_continuation_starts(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, "aaaa bbbb @p cccc - dd 1. ee", strlen("aaaa bbbb @p cccc - dd 1. ee")},
  };
  struct BodyLineList lines = {items, 1};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 12, false, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(lines.count == 4);
  TEST_CHECK(strcmp(lines.items[0].text, "aaaa") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "bbbb @p") == 0);
  TEST_CHECK(strcmp(lines.items[2].text, "cccc -") == 0);
  TEST_CHECK(strcmp(lines.items[3].text, "dd 1. ee") == 0);
  arena_free(&arena);
}

// A tool directive inside prose travels with the word before it, so no continuation line starts
// with one and reads back as a directive line.
static void test_keeps_tool_directives_off_continuation_starts(void) {
  struct Arena arena;
  arena_init(&arena);
  char text[] = "aaaa NOLINT bbbb clang-format off";
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, text, strlen(text)},
  };
  struct BodyLineList lines = {items, 1};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 12, false, &arena, err, sizeof(err)) == 0);
  TEST_ASSERT(lines.count == 3);
  TEST_CHECK(strcmp(lines.items[0].text, "aaaa NOLINT") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "bbbb clang-format") == 0);
  TEST_CHECK(strcmp(lines.items[2].text, "off") == 0);
  arena_free(&arena);
}

// A directive line wider than the column is copied as is and splits the prose around it.
static void test_leaves_directive_lines_untouched(void) {
  struct Arena arena;
  arena_init(&arena);
  char one[] = "one";
  char directive[] = "NOLINTNEXTLINE(misc-no-recursion)";
  char two[] = "two";
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, one, 3},
      {BODY_LINE_DIRECTIVE, directive, strlen(directive)},
      {BODY_LINE_PROSE, two, 3},
  };
  struct BodyLineList lines = {items, 3};
  char err[64];
  TEST_CHECK(fill_body_lines(&lines, 3, 3, 20, false, &arena, err, sizeof(err)) == 0);
  TEST_ASSERT(lines.count == 3);
  TEST_CHECK(strcmp(lines.items[0].text, "one") == 0);
  TEST_CHECK(lines.items[1].kind == BODY_LINE_DIRECTIVE);
  TEST_CHECK(strcmp(lines.items[1].text, directive) == 0);
  TEST_CHECK(strcmp(lines.items[2].text, "two") == 0);
  arena_free(&arena);
}

TEST_LIST = {
    {"joins short prose lines", test_joins_short_prose_lines},
    {"leaves list items untouched", test_leaves_list_items_untouched},
    {"indents wrapped tag description", test_indents_wrapped_tag_description},
    {"refills tag continuation", test_refills_tag_continuation},
    {"tag line starts new paragraph", test_tag_line_starts_new_paragraph},
    {"rebuilds multiple paragraphs in one pass", test_rebuilds_multiple_paragraphs_in_one_pass},
    {"keeps line openers off continuation starts", test_keeps_line_openers_off_continuation_starts},
    {"keeps tool directives off continuation starts",
     test_keeps_tool_directives_off_continuation_starts},
    {"leaves directive lines untouched", test_leaves_directive_lines_untouched},
    {NULL, NULL},
};
