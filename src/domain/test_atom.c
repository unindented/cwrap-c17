#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "domain/atom.h"
#include "shared/arena.h"

// Display width counts East Asian wide code points as two columns.
static void test_column_width_east_asian_and_latin(void) {
  TEST_CHECK(atom_column_width("日本語", strlen("日本語")) == 6);
  TEST_CHECK(atom_column_width("😀", strlen("😀")) == 2);
  TEST_CHECK(atom_column_width("𠀀", strlen("𠀀")) == 2);
  TEST_CHECK(atom_column_width("é", strlen("é")) == 1);
  TEST_CHECK(atom_column_width("—", strlen("—")) == 1);
  TEST_CHECK(atom_column_width("abc", 3) == 3);
}

// Unicode 16.0 combining marks and format characters occupy no independent display column.
static void test_column_width_zero_width_codepoints(void) {
  TEST_CHECK(atom_column_width("が", strlen("が")) == 2);
  TEST_CHECK(atom_column_width("a҃", strlen("a҃")) == 1);
  TEST_CHECK(atom_column_width("\xE2\x80\x8D", 3) == 0);
}

// Invalid UTF-8 bytes are measured independently rather than decoded as an overlong control.
static void test_column_width_invalid_utf8(void) {
  const char overlong_nul[] = {(char)0xC0, (char)0x80};
  TEST_CHECK(atom_column_width(overlong_nul, sizeof(overlong_nul)) == 2);
}

// A backtick span, hex run, and quantity-plus-unit stay one atom. A parenthetical does not.
static void test_split_keeps_unbreakable_atoms(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* text = "see `const char*` and E2 80 94 (24 of 32 slots) plus 264 MB here";
  struct AtomList atoms;
  TEST_CHECK(atom_split(text, strlen(text), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count >= 5);
  TEST_CHECK(atoms.items[1].text_len == strlen("`const char*`"));
  TEST_CHECK(memcmp(atoms.items[1].text, "`const char*`", atoms.items[1].text_len) == 0);

  bool has_hex = false;
  bool has_quantity = false;
  bool has_whole_parenthetical = false;
  bool has_open_parenthetical = false;
  for (size_t i = 0; i < atoms.count; i++) {
    if (atoms.items[i].text_len == 8 && memcmp(atoms.items[i].text, "E2 80 94", 8) == 0) {
      has_hex = true;
    }
    if (atoms.items[i].text_len == 16 && memcmp(atoms.items[i].text, "(24 of 32 slots)", 16) == 0) {
      has_whole_parenthetical = true;
    }
    if (atoms.items[i].text_len == 3 && memcmp(atoms.items[i].text, "(24", 3) == 0) {
      has_open_parenthetical = true;
    }
    if (atoms.items[i].text_len == 6 && memcmp(atoms.items[i].text, "264 MB", 6) == 0) {
      has_quantity = true;
    }
  }
  TEST_CHECK(has_hex);
  TEST_CHECK(has_quantity);
  TEST_CHECK(has_open_parenthetical);
  TEST_CHECK(!has_whole_parenthetical);
  arena_free(&arena);
}

// Lowercase hex-looking words stay ordinary words. Uppercase byte runs stay one atom.
static void test_split_hex_is_uppercase_only(void) {
  struct Arena arena;
  arena_init(&arena);
  struct AtomList atoms;
  TEST_CHECK(atom_split("be ad", 5, &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(atom_split("BE AD", 5, &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 1);
  TEST_CHECK(atoms.items[0].text_len == 5);
  arena_free(&arena);
}

// `10 and` is English. `264 MB` is a quantity.
static void test_split_quantity_requires_known_unit(void) {
  struct Arena arena;
  arena_init(&arena);
  struct AtomList atoms;
  TEST_CHECK(atom_split("10 and then", strlen("10 and then"), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 3);
  TEST_CHECK(atoms.items[0].text_len == 2);
  TEST_CHECK(memcmp(atoms.items[0].text, "10", 2) == 0);
  TEST_CHECK(atom_split("264 MB here", strlen("264 MB here"), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(atoms.items[0].text_len == 6);
  TEST_CHECK(memcmp(atoms.items[0].text, "264 MB", 6) == 0);
  arena_free(&arena);
}

// Two hex-looking English words are not a byte run. `be able` must split, or fill wraps before `be`
// when the glued atom plus a hanging closer will not fit.
static void test_split_does_not_glue_be_able(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* text = "must not be able";
  struct AtomList atoms;
  TEST_CHECK(atom_split(text, strlen(text), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 4);
  TEST_CHECK(atoms.items[2].text_len == 2);
  TEST_CHECK(memcmp(atoms.items[2].text, "be", 2) == 0);
  TEST_CHECK(atoms.items[3].text_len == 4);
  TEST_CHECK(memcmp(atoms.items[3].text, "able", 4) == 0);
  arena_free(&arena);
}

// A possessive, a trailing period, and a hyphenated suffix travel with the preceding atom.
static void test_split_attaches_possessive_period_and_hyphen(void) {
  struct Arena arena;
  arena_init(&arena);
  const char* possessive = "`mustache.c`'s.";
  struct AtomList atoms;
  TEST_CHECK(atom_split(possessive, strlen(possessive), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 1);
  TEST_CHECK(atoms.items[0].text_len == strlen(possessive));

  const char* compound = "`max_align_t`-aligned so";
  TEST_CHECK(atom_split(compound, strlen(compound), &arena, &atoms) == 0);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(atoms.items[0].text_len == strlen("`max_align_t`-aligned"));
  TEST_CHECK(memcmp(atoms.items[0].text, "`max_align_t`-aligned", atoms.items[0].text_len) == 0);
  arena_free(&arena);
}

TEST_LIST = {
    {"column width east asian and latin", test_column_width_east_asian_and_latin},
    {"column width zero width codepoints", test_column_width_zero_width_codepoints},
    {"column width invalid utf8", test_column_width_invalid_utf8},
    {"split keeps unbreakable atoms", test_split_keeps_unbreakable_atoms},
    {"split hex is uppercase only", test_split_hex_is_uppercase_only},
    {"split quantity requires known unit", test_split_quantity_requires_known_unit},
    {"split does not glue be able", test_split_does_not_glue_be_able},
    {"split attaches possessive period and hyphen",
     test_split_attaches_possessive_period_and_hyphen},
    {NULL, NULL},
};
