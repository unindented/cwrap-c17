#include <acutest.h>
#include <stddef.h>

#include "domain/comment.h"
#include "domain/doxygen.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// A command's word, name, and description offset are sliced from the payload.
static void test_parses_named_tag(void) {
  struct DoxygenTag tag;
  TEST_CHECK(doxygen_has_tag(S("@param[in] name Description."), &tag));
  TEST_CHECK(str_eq(tag.command, S("param")));
  TEST_CHECK(tag.keyword_len == S("@param[in]").len);
  TEST_CHECK(str_eq(tag.name, S("name")));
  TEST_CHECK(tag.description_offset == S("@param[in] name ").len);
}

// Any command word is a tag, in either spelling, and only `param` and `retval` take a name.
static void test_parses_any_command_word(void) {
  static const struct Str texts[] = {STR_INIT("\\brief Summary."), STR_INIT("@note Summary."),
                                     STR_INIT("@code{.c}"), STR_INIT("@endcode")};
  static const struct Str commands[] = {STR_INIT("brief"), STR_INIT("note"), STR_INIT("code"),
                                        STR_INIT("endcode")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    struct DoxygenTag tag;
    TEST_CHECK(doxygen_has_tag(texts[i], &tag));
    TEST_CHECK(str_eq(tag.command, commands[i]));
    TEST_CHECK(tag.name.len == 0);
  }
}

// A tag is parsed only from the bytes of its view: an option the view cuts short is unclosed, and a
// name the view ends before is not there.
static void test_parses_tag_within_view_only(void) {
  struct DoxygenTag tag;
  TEST_CHECK(!doxygen_has_tag(str_head(S("@param[in] x"), 8), &tag));
  TEST_CHECK(doxygen_has_tag(str_head(S("@param x"), 7), &tag));
  TEST_CHECK(str_eq(tag.command, S("param")));
  TEST_CHECK(tag.name.len == 0);
}

// `@param` descriptions share a column taken from the longest name.
static void test_aligns_parameter_descriptions(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param x yes")},
      {BODY_LINE_TAG, S("@param name no")},
  };
  struct BodyLineList lines = {items, 2};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param x    yes")));
  TEST_CHECK(str_eq(lines.items[1].text, S("@param name no")));
}

// Direction-qualified parameters align by name while preserving each qualifier.
static void test_aligns_direction_qualified_parameters(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param[in] x first")},
      {BODY_LINE_TAG, S("@param[out] longer second")},
  };
  struct BodyLineList lines = {items, 2};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param[in] x      first")));
  TEST_CHECK(str_eq(lines.items[1].text, S("@param[out] longer second")));
}

// Parameter alignment measures display columns and does not pad a tag with no description.
static void test_aligns_unicode_names_and_leaves_empty_description_unpadded(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param é first")},
      {BODY_LINE_TAG, S("@param name second")},
      {BODY_LINE_TAG, S("@param x")},
  };
  struct BodyLineList lines = {items, 3};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param é    first")));
  TEST_CHECK(str_eq(lines.items[1].text, S("@param name second")));
  TEST_CHECK(str_eq(lines.items[2].text, S("@param x")));
}

// `@retval` values align among themselves, apart from `@param` names, in either spelling.
static void test_aligns_retval_separately_from_param(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param buffer out")},
      {BODY_LINE_TAG, S("\\retval 0 ok")},
      {BODY_LINE_TAG, S("@retval -1 failed")},
  };
  struct BodyLineList lines = {items, 3};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param buffer out")));
  TEST_CHECK(str_eq(lines.items[1].text, S("\\retval 0  ok")));
  TEST_CHECK(str_eq(lines.items[2].text, S("@retval -1 failed")));
}

// Each command keeps its own column when the two alternate, so the widest name is measured for the
// command of each line, not carried over from the line before.
static void test_aligns_alternating_commands_separately(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param a x")},
      {BODY_LINE_TAG, S("@retval 0 ok")},
      {BODY_LINE_TAG, S("@param long y")},
      {BODY_LINE_TAG, S("@retval -1 failed")},
  };
  struct BodyLineList lines = {items, 4};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param a    x")));
  TEST_CHECK(str_eq(lines.items[1].text, S("@retval 0  ok")));
  TEST_CHECK(str_eq(lines.items[2].text, S("@param long y")));
  TEST_CHECK(str_eq(lines.items[3].text, S("@retval -1 failed")));
}

// Many `@param` lines all align on the column of the widest name, whichever line holds it.
static void test_aligns_many_parameters(void) {
  enum { LINE_COUNT = 20000 };
  struct Arena arena = test_arena();
  struct BodyLine* items = arena_new(&arena, LINE_COUNT, struct BodyLine);
  for (ptrdiff_t i = 0; i < LINE_COUNT; i++) {
    items[i] = (struct BodyLine){BODY_LINE_TAG, i == 0 ? S("@param name d") : S("@param a d")};
  }
  struct BodyLineList lines = {items, LINE_COUNT};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param name d")));
  TEST_CHECK(str_eq(lines.items[LINE_COUNT - 1].text, S("@param a    d")));
}

// A description that starts on the continuation line is padded so the single joining space lands it
// on the shared column.
static void test_pads_description_on_continuation_line(void) {
  struct Arena arena = test_arena();
  struct BodyLine items[] = {
      {BODY_LINE_TAG, S("@param x")},
      {BODY_LINE_PROSE, S("Value.")},
      {BODY_LINE_TAG, S("@param name no")},
  };
  struct BodyLineList lines = {items, 3};
  doxygen_align_parameters(&lines, &arena);
  TEST_CHECK(str_eq(lines.items[0].text, S("@param x   ")));
  TEST_CHECK(str_eq(lines.items[1].text, S("Value.")));
}

// A marker without a letter, a word glued to punctuation, and an unclosed option are not commands.
static void test_rejects_non_command_words(void) {
  static const struct Str texts[] = {STR_INIT("@"),           STR_INIT("@2x wide"),
                                     STR_INIT("\\0 byte"),    STR_INIT("@p, then"),
                                     STR_INIT("@param[in x"), STR_INIT("user@host")};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    struct DoxygenTag tag;
    TEST_CHECK(!doxygen_has_tag(texts[i], &tag));
    TEST_MSG("text: '%.*s'", STR_FMT(texts[i]));
  }
}

TEST_LIST = {
    {"parses named tag", test_parses_named_tag},
    {"parses any command word", test_parses_any_command_word},
    {"parses tag within view only", test_parses_tag_within_view_only},
    {"aligns parameter descriptions", test_aligns_parameter_descriptions},
    {"aligns direction qualified parameters", test_aligns_direction_qualified_parameters},
    {"aligns unicode names and leaves empty description unpadded",
     test_aligns_unicode_names_and_leaves_empty_description_unpadded},
    {"aligns retval separately from param", test_aligns_retval_separately_from_param},
    {"aligns alternating commands separately", test_aligns_alternating_commands_separately},
    {"aligns many parameters", test_aligns_many_parameters},
    {"pads description on continuation line", test_pads_description_on_continuation_line},
    {"rejects non command words", test_rejects_non_command_words},
    {NULL, NULL},
};
