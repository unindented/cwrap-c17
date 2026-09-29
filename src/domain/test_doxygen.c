#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "domain/comment.h"
#include "domain/doxygen.h"
#include "shared/arena.h"

// A command's word, name, and description offset are sliced from the payload.
static void test_parses_named_tag(void) {
  const char* text = "@param[in] name Description.";
  struct DoxygenTag tag;
  TEST_CHECK(doxygen_has_tag(text, strlen(text), &tag));
  TEST_CHECK(tag.command_len == 5 && strncmp(tag.command, "param", 5) == 0);
  TEST_CHECK(tag.keyword_len == strlen("@param[in]"));
  TEST_CHECK(tag.name_len == 4 && strncmp(tag.name, "name", 4) == 0);
  TEST_CHECK(tag.description_offset == strlen("@param[in] name "));
}

// Any command word is a tag, in either spelling, and only `param` and `retval` take a name.
static void test_parses_any_command_word(void) {
  const char* texts[] = {"\\brief Summary.", "@note Summary.", "@code{.c}", "@endcode"};
  const size_t command_lens[] = {5, 4, 4, 7};
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    struct DoxygenTag tag;
    TEST_CHECK(doxygen_has_tag(texts[i], strlen(texts[i]), &tag));
    TEST_CHECK(tag.command_len == command_lens[i]);
    TEST_CHECK(tag.name == NULL);
  }
}

// `@param` descriptions share a column taken from the longest name.
static void test_aligns_parameter_descriptions(void) {
  struct Arena arena;
  arena_init(&arena);
  char short_name[] = "@param x yes";
  char long_name[] = "@param name no";
  struct BodyLine items[] = {
      {BODY_LINE_TAG, short_name, strlen(short_name)},
      {BODY_LINE_TAG, long_name, strlen(long_name)},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[0].text, "@param x    yes") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "@param name no") == 0);
  arena_free(&arena);
}

// Direction-qualified parameters align by name while preserving each qualifier.
static void test_aligns_direction_qualified_parameters(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_TAG, "@param[in] x first", strlen("@param[in] x first")},
      {BODY_LINE_TAG, "@param[out] longer second", strlen("@param[out] longer second")},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[0].text, "@param[in] x      first") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "@param[out] longer second") == 0);
  arena_free(&arena);
}

// Parameter alignment measures display columns and does not pad a tag with no description.
static void test_aligns_unicode_names_and_leaves_empty_description_unpadded(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_TAG, "@param é first", strlen("@param é first")},
      {BODY_LINE_TAG, "@param name second", strlen("@param name second")},
      {BODY_LINE_TAG, "@param x", strlen("@param x")},
  };
  struct BodyLineList lines = {items, 3};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[0].text, "@param é    first") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "@param name second") == 0);
  TEST_CHECK(strcmp(lines.items[2].text, "@param x") == 0);
  arena_free(&arena);
}

// `@retval` values align among themselves, apart from `@param` names, in either spelling.
static void test_aligns_retval_separately_from_param(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_TAG, "@param buffer out", strlen("@param buffer out")},
      {BODY_LINE_TAG, "\\retval 0 ok", strlen("\\retval 0 ok")},
      {BODY_LINE_TAG, "@retval -1 failed", strlen("@retval -1 failed")},
  };
  struct BodyLineList lines = {items, 3};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[0].text, "@param buffer out") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "\\retval 0  ok") == 0);
  TEST_CHECK(strcmp(lines.items[2].text, "@retval -1 failed") == 0);
  arena_free(&arena);
}

// A description that starts on the continuation line is padded so the single joining space lands it
// on the shared column.
static void test_pads_description_on_continuation_line(void) {
  struct Arena arena;
  arena_init(&arena);
  struct BodyLine items[] = {
      {BODY_LINE_TAG, "@param x", strlen("@param x")},
      {BODY_LINE_PROSE, "Value.", strlen("Value.")},
      {BODY_LINE_TAG, "@param name no", strlen("@param name no")},
  };
  struct BodyLineList lines = {items, 3};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[0].text, "@param x   ") == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "Value.") == 0);
  arena_free(&arena);
}

// A marker without a letter, a word glued to punctuation, and an unclosed option are not commands.
static void test_rejects_non_command_words(void) {
  const char* texts[] = {"@", "@2x wide", "\\0 byte", "@p, then", "@param[in x", "user@host"};
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    struct DoxygenTag tag;
    TEST_CHECK(!doxygen_has_tag(texts[i], strlen(texts[i]), &tag));
    TEST_MSG("text: '%s'", texts[i]);
  }
}

TEST_LIST = {
    {"parses named tag", test_parses_named_tag},
    {"parses any command word", test_parses_any_command_word},
    {"aligns parameter descriptions", test_aligns_parameter_descriptions},
    {"aligns direction qualified parameters", test_aligns_direction_qualified_parameters},
    {"aligns unicode names and leaves empty description unpadded",
     test_aligns_unicode_names_and_leaves_empty_description_unpadded},
    {"aligns retval separately from param", test_aligns_retval_separately_from_param},
    {"pads description on continuation line", test_pads_description_on_continuation_line},
    {"rejects non command words", test_rejects_non_command_words},
    {NULL, NULL},
};
