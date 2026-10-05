#include <acutest.h>

#include "core/ascii.h"

// `ascii_is_digit` answers for `[0-9]` and nothing else, asserted at both edges. Only `parse_size`
// re-checks the byte afterwards by handing it to `strtoull`. The other callers do not: widening the
// set would change which spans `quantity_unit_len` treats as a quantity, which bytes `is_hex` lets
// into a byte run, and which payload lines `comment_is_list_line` treats as a numbered marker, all
// of which are golden-fixture behavior.
static void test_is_digit_accepts_only_ascii_digits(void) {
  TEST_CHECK(ascii_is_digit('0'));
  TEST_CHECK(ascii_is_digit('5'));
  TEST_CHECK(ascii_is_digit('9'));

  // The bytes immediately outside the range on each side.
  TEST_CHECK(!ascii_is_digit('/'));
  TEST_CHECK(!ascii_is_digit(':'));
  TEST_CHECK(!ascii_is_digit('a'));
  TEST_CHECK(!ascii_is_digit(' '));
  TEST_CHECK(!ascii_is_digit('\0'));
  TEST_CHECK(!ascii_is_digit(0xC3));
}

// `ascii_is_alphanumeric` answers for `[0-9A-Za-z]` and nothing else, asserted at every edge of the
// three ranges. This predicate defines the byte-run end of `hex_run_len`, which stops `be able`
// from parsing as the run `be ab`, the letter-or-digit test by which `has_alphanumeric_payload` and
// `is_decoration_line` leave a block or payload line unwrapped, and the word boundary at which
// `is_word_byte` ends a directive, so widening it by one byte silently changes all four.
static void test_is_alnum_accepts_only_letters_and_digits(void) {
  TEST_CHECK(ascii_is_alphanumeric('0'));
  TEST_CHECK(ascii_is_alphanumeric('9'));
  TEST_CHECK(ascii_is_alphanumeric('A'));
  TEST_CHECK(ascii_is_alphanumeric('Z'));
  TEST_CHECK(ascii_is_alphanumeric('a'));
  TEST_CHECK(ascii_is_alphanumeric('z'));

  // The byte on each side of each range: '/'/':' bracket the digits, '@'/'[' the uppercase letters,
  // '`'/'{' the lowercase ones.
  TEST_CHECK(!ascii_is_alphanumeric('/'));
  TEST_CHECK(!ascii_is_alphanumeric(':'));
  TEST_CHECK(!ascii_is_alphanumeric('@'));
  TEST_CHECK(!ascii_is_alphanumeric('['));
  TEST_CHECK(!ascii_is_alphanumeric('`'));
  TEST_CHECK(!ascii_is_alphanumeric('{'));

  // The `_` that `is_word_byte` allows separately, which this predicate must not fold in itself,
  // plus other punctuation and the ends of the byte range.
  TEST_CHECK(!ascii_is_alphanumeric('_'));
  TEST_CHECK(!ascii_is_alphanumeric('-'));
  TEST_CHECK(!ascii_is_alphanumeric('.'));
  TEST_CHECK(!ascii_is_alphanumeric(' '));
  TEST_CHECK(!ascii_is_alphanumeric('\0'));
  TEST_CHECK(!ascii_is_alphanumeric(0x7F));
  TEST_CHECK(!ascii_is_alphanumeric(0xC3));
  TEST_CHECK(!ascii_is_alphanumeric(0xFF));
}

TEST_LIST = {
    {"is digit accepts only ascii digits", test_is_digit_accepts_only_ascii_digits},
    {"is alnum accepts only letters and digits", test_is_alnum_accepts_only_letters_and_digits},
    {NULL, NULL},
};
