#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "domain/comment.h"
#include "domain/doxygen.h"

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

// A continuation is indented to the nearest preceding tag's description column.
static void test_indents_continuations_to_tag_column(void) {
  struct Arena arena;
  arena_init(&arena);
  char tag[] = "@return the value";
  char continuation[] = "more";
  struct BodyLine items[] = {
      {BODY_LINE_TAG, tag, strlen(tag)},
      {BODY_LINE_PROSE, continuation, 4},
  };
  struct BodyLineList lines = {items, 2};
  char err[64];
  TEST_CHECK(doxygen_align_parameters(&lines, &arena, err, sizeof(err)) == 0);
  TEST_CHECK(strcmp(lines.items[1].text, "        more") == 0);
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

TEST_LIST = {
    {"aligns parameter descriptions", test_aligns_parameter_descriptions},
    {"indents continuations to tag column", test_indents_continuations_to_tag_column},
    {"aligns direction qualified parameters", test_aligns_direction_qualified_parameters},
    {"aligns unicode names and leaves empty description unpadded",
     test_aligns_unicode_names_and_leaves_empty_description_unpadded},
    {NULL, NULL},
};
