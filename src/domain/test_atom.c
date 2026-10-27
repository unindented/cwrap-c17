#include <acutest.h>
#include <stddef.h>

#include "domain/atom.h"
#include "shared/arena.h"
#include "shared/str.h"
#include "test_support.h"

// Display width counts East Asian wide code points as two columns.
static void test_column_width_east_asian_and_latin(void) {
  TEST_CHECK(atom_column_width(S("日本語")) == 6);
  TEST_CHECK(atom_column_width(S("😀")) == 2);
  TEST_CHECK(atom_column_width(S("𠀀")) == 2);
  TEST_CHECK(atom_column_width(S("é")) == 1);
  TEST_CHECK(atom_column_width(S("—")) == 1);
  TEST_CHECK(atom_column_width(S("abc")) == 3);
}

// Unicode 16.0 combining marks and format characters occupy no independent display column.
static void test_column_width_zero_width_codepoints(void) {
  TEST_CHECK(atom_column_width(S("が")) == 2);
  TEST_CHECK(atom_column_width(S("a҃")) == 1);
  TEST_CHECK(atom_column_width(S("\xE2\x80\x8D")) == 0);
}

// Invalid UTF-8 bytes are measured independently rather than decoded as an overlong control.
static void test_column_width_invalid_utf8(void) {
  TEST_CHECK(atom_column_width(S("\xC0\x80")) == 2);
}

// A code point cut short by the end of the text is measured byte by byte, and nothing past the end
// is read to complete it.
static void test_column_width_stops_at_view_end(void) {
  TEST_CHECK(atom_column_width(str_head(S("日本"), 4)) == 3);
}

// A backtick span, hex run, and quantity-plus-unit stay one atom. A parenthetical does not.
static void test_split_keeps_unbreakable_atoms(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms =
      atom_split(S("see `const char*` and E2 80 94 (24 of 32 slots) plus 264 MB here"), &arena);
  static const struct Str expected[] = {
      STR_INIT("see"),      STR_INIT("`const char*`"), STR_INIT("and"),
      STR_INIT("E2 80 94"), STR_INIT("(24"),           STR_INIT("of"),
      STR_INIT("32"),       STR_INIT("slots)"),        STR_INIT("plus"),
      STR_INIT("264 MB"),   STR_INIT("here"),
  };
  const ptrdiff_t expected_count = (ptrdiff_t)(sizeof(expected) / sizeof(expected[0]));
  TEST_CHECK(atoms.count == expected_count);
  for (ptrdiff_t i = 0; i < atoms.count && i < expected_count; i++) {
    TEST_CHECK(str_eq(atoms.items[i].text, expected[i]));
    TEST_MSG("atom %td: expected '%.*s'", i, STR_FMT(expected[i]));
  }
}

// Lowercase hex-looking words stay ordinary words. Uppercase byte runs stay one atom.
static void test_split_hex_is_uppercase_only(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms = atom_split(S("be ad"), &arena);
  TEST_CHECK(atoms.count == 2);
  atoms = atom_split(S("BE AD"), &arena);
  TEST_CHECK(atoms.count == 1);
  TEST_CHECK(str_eq(atoms.items[0].text, S("BE AD")));
}

// `10 and` is English. `264 MB` is a quantity.
static void test_split_quantity_requires_known_unit(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms = atom_split(S("10 and then"), &arena);
  TEST_CHECK(atoms.count == 3);
  TEST_CHECK(str_eq(atoms.items[0].text, S("10")));
  atoms = atom_split(S("264 MB here"), &arena);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(str_eq(atoms.items[0].text, S("264 MB")));
}

// A quantity whose unit the end of the text cuts short is an ordinary word, because the view ends
// where its bytes do, not where a terminator would.
static void test_split_quantity_stops_at_view_end(void) {
  struct Arena arena = test_arena();
  const struct AtomList atoms = atom_split(str_head(S("264 MB"), 5), &arena);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(str_eq(atoms.items[0].text, S("264")));
  TEST_CHECK(str_eq(atoms.items[1].text, S("M")));
}

// Two hex-looking English words are not a byte run. `be able` must split, or fill wraps before `be`
// when the glued atom plus a hanging closer will not fit.
static void test_split_does_not_glue_be_able(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms = atom_split(S("must not be able"), &arena);
  TEST_CHECK(atoms.count == 4);
  TEST_CHECK(str_eq(atoms.items[2].text, S("be")));
  TEST_CHECK(str_eq(atoms.items[3].text, S("able")));
}

// A backtick span that starts a word or follows an opening bracket stays one atom with that word,
// spaces included, so its closer cannot open a span of its own, even when it lands after a bracket.
// A backtick after any other byte, as in TeX's \char`x, or quoted between brackets opens nothing.
// `tests/fixtures/backtick-spans.c` covers how this wraps.
static void test_split_keeps_span_after_opening_bracket(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms = atom_split(S("up (`\\ `) is, joined with `, ` and {`\\ `} or [`a b`], "
                                       "then \\char`x a` (`) b `)` `(`/`{` z"),
                                     &arena);
  static const struct Str expected[] = {
      STR_INIT("up"),      STR_INIT("(`\\ `)"),  STR_INIT("is,"),  STR_INIT("joined"),
      STR_INIT("with"),    STR_INIT("`, `"),     STR_INIT("and"),  STR_INIT("{`\\ `}"),
      STR_INIT("or"),      STR_INIT("[`a b`],"), STR_INIT("then"), STR_INIT("\\char`x"),
      STR_INIT("a`"),      STR_INIT("(`)"),      STR_INIT("b"),    STR_INIT("`)`"),
      STR_INIT("`(`/`{`"), STR_INIT("z"),
  };
  const ptrdiff_t expected_count = (ptrdiff_t)(sizeof(expected) / sizeof(expected[0]));
  TEST_CHECK(atoms.count == expected_count);
  for (ptrdiff_t i = 0; i < atoms.count && i < expected_count; i++) {
    TEST_CHECK(str_eq(atoms.items[i].text, expected[i]));
    TEST_MSG("atom %td: expected '%.*s'", i, STR_FMT(expected[i]));
  }
}

// A possessive, a trailing period, and a hyphenated suffix travel with the preceding atom.
static void test_split_attaches_possessive_period_and_hyphen(void) {
  struct Arena arena = test_arena();
  struct AtomList atoms = atom_split(S("`comment.c`'s."), &arena);
  TEST_CHECK(atoms.count == 1);
  TEST_CHECK(str_eq(atoms.items[0].text, S("`comment.c`'s.")));

  atoms = atom_split(S("`max_align_t`-aligned so"), &arena);
  TEST_CHECK(atoms.count == 2);
  TEST_CHECK(str_eq(atoms.items[0].text, S("`max_align_t`-aligned")));
}

TEST_LIST = {
    {"column width east asian and latin", test_column_width_east_asian_and_latin},
    {"column width zero width codepoints", test_column_width_zero_width_codepoints},
    {"column width invalid utf8", test_column_width_invalid_utf8},
    {"column width stops at view end", test_column_width_stops_at_view_end},
    {"split keeps unbreakable atoms", test_split_keeps_unbreakable_atoms},
    {"split hex is uppercase only", test_split_hex_is_uppercase_only},
    {"split quantity requires known unit", test_split_quantity_requires_known_unit},
    {"split quantity stops at view end", test_split_quantity_stops_at_view_end},
    {"split does not glue be able", test_split_does_not_glue_be_able},
    {"split keeps span after opening bracket", test_split_keeps_span_after_opening_bracket},
    {"split attaches possessive period and hyphen",
     test_split_attaches_possessive_period_and_hyphen},
    {NULL, NULL},
};
