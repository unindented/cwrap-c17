#include <acutest.h>
#include <stddef.h>

#include "domain/comment.h"
#include "domain/fill.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// A short pair of prose lines is joined when both fit on one line.
static void test_joins_short_prose_lines(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("one")},
      {BODY_LINE_PROSE, S("two")},
  };
  struct BodyLineList lines = {items, 2};
  fill_body_lines(&lines, 3, 3, 20, false, &arena);
  TEST_CHECK(lines.count == 1);
  TEST_CHECK(str_eq(lines.items[0].text, S("one two")));
}

// A list item is left untouched even when the line is short.
static void test_leaves_list_items_untouched(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_LIST, S("- first")},
      {BODY_LINE_PROSE, S("second")},
  };
  struct BodyLineList lines = {items, 2};
  fill_body_lines(&lines, 3, 3, 40, false, &arena);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(str_eq(lines.items[0].text, S("- first")));
  TEST_CHECK(str_eq(lines.items[1].text, S("second")));
}

// A wrapped tag description continues at the description column.
static void test_indents_wrapped_tag_description(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param longername short more of the last tag")},
  };
  struct BodyLineList lines = {items, 1};
  fill_body_lines(&lines, 3, 3, 40, false, &arena);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param longername short more of the")));
  TEST_CHECK(str_eq(lines.items[1].text, S("                  last tag")));
}

// A tag's continuation joins it, and the refilled description hangs under its first word.
static void test_refills_tag_continuation(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@note one two")},
      {BODY_LINE_PROSE, S("three four five")},
  };
  struct BodyLineList lines = {items, 2};
  fill_body_lines(&lines, 3, 3, 24, false, &arena);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(lines.items[0].kind == BODY_LINE_TAG);
  TEST_CHECK(str_eq(lines.items[0].text, S("@note one two three")));
  TEST_CHECK(lines.items[1].kind == BODY_LINE_PROSE);
  TEST_CHECK(str_eq(lines.items[1].text, S("      four five")));
}

// A tag line starts its own paragraph and is not joined with the prose above.
static void test_tag_line_starts_new_paragraph(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("Description.")},
      {BODY_LINE_TAG, S("@param x value")},
  };
  struct BodyLineList lines = {items, 2};
  fill_body_lines(&lines, 3, 3, 40, false, &arena);
  TEST_CHECK(lines.count == 2);
  TEST_CHECK(str_eq(lines.items[0].text, S("Description.")));
  TEST_CHECK(str_eq(lines.items[1].text, S("@param x value")));
}

// Multiple paragraphs are rebuilt once while blank and list lines retain their order.
static void test_rebuilds_multiple_paragraphs_in_one_pass(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("one")},   {BODY_LINE_PROSE, S("two")},  {BODY_LINE_BLANK, S("")},
      {BODY_LINE_PROSE, S("three")}, {BODY_LINE_PROSE, S("four")}, {BODY_LINE_BLANK, S("")},
      {BODY_LINE_LIST, S("- item")},
  };
  struct BodyLineList lines = {items, 7};
  fill_body_lines(&lines, 3, 3, 40, false, &arena);
  TEST_CHECK(lines.count == 5);
  TEST_CHECK(str_eq(lines.items[0].text, S("one two")));
  TEST_CHECK(lines.items[1].kind == BODY_LINE_BLANK);
  TEST_CHECK(str_eq(lines.items[2].text, S("three four")));
  TEST_CHECK(lines.items[3].kind == BODY_LINE_BLANK);
  TEST_CHECK(str_eq(lines.items[4].text, S("- item")));
}

// A command word or list marker travels with the word before it, so no continuation line starts
// with one and reads back as a tag or list item.
static void test_keeps_line_openers_off_continuation_starts(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("aaaa bbbb @p cccc - dd 1. ee")},
  };
  struct BodyLineList lines = {items, 1};
  fill_body_lines(&lines, 3, 3, 12, false, &arena);
  TEST_CHECK(lines.count == 4);
  TEST_CHECK(str_eq(lines.items[0].text, S("aaaa")));
  TEST_CHECK(str_eq(lines.items[1].text, S("bbbb @p")));
  TEST_CHECK(str_eq(lines.items[2].text, S("cccc -")));
  TEST_CHECK(str_eq(lines.items[3].text, S("dd 1. ee")));
}

// A tool directive inside prose travels with the word before it, so no continuation line starts
// with one and reads back as a directive line.
static void test_keeps_tool_directives_off_continuation_starts(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("aaaa NOLINT bbbb clang-format off")},
  };
  struct BodyLineList lines = {items, 1};
  fill_body_lines(&lines, 3, 3, 12, false, &arena);
  TEST_ASSERT(lines.count == 3);
  TEST_CHECK(str_eq(lines.items[0].text, S("aaaa NOLINT")));
  TEST_CHECK(str_eq(lines.items[1].text, S("bbbb clang-format")));
  TEST_CHECK(str_eq(lines.items[2].text, S("off")));
}

// A directive line wider than the column is copied as is and splits the prose around it.
static void test_leaves_directive_lines_untouched(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_PROSE, S("one")},
      {BODY_LINE_DIRECTIVE, S("NOLINTNEXTLINE(misc-no-recursion)")},
      {BODY_LINE_PROSE, S("two")},
  };
  struct BodyLineList lines = {items, 3};
  fill_body_lines(&lines, 3, 3, 20, false, &arena);
  TEST_ASSERT(lines.count == 3);
  TEST_CHECK(str_eq(lines.items[0].text, S("one")));
  TEST_CHECK(lines.items[1].kind == BODY_LINE_DIRECTIVE);
  TEST_CHECK(str_eq(lines.items[1].text, S("NOLINTNEXTLINE(misc-no-recursion)")));
  TEST_CHECK(str_eq(lines.items[2].text, S("two")));
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
