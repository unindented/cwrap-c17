#include <acutest.h>

#include "core/ascii.h"

// `ascii_is_digit` answers for `[0-9]` and nothing else, asserted at both edges. Only `parse_size`
// re-checks the byte afterwards by handing it to `strtoull`. The other callers do not: widening the
// set would change which spans `quantity_unit_len` treats as a quantity and which payload lines
// `is_list_line` treats as a numbered marker, both of which are golden-fixture behavior.
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
// three ranges. Two callers depend on the exact set. `hex_run_len` uses it to end a byte run, which
// is what stops `be able` from parsing as the run `be ab`, and `is_decoration_line` uses it to
// decide a payload carries no letters or digits and is therefore left unwrapped. Widening it by one
// byte changes both, silently.
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

  // The punctuation the path and identifier rules allow separately, which this predicate must not
  // fold in itself, plus the ends of the byte range.
  TEST_CHECK(!ascii_is_alphanumeric('_'));
  TEST_CHECK(!ascii_is_alphanumeric('-'));
  TEST_CHECK(!ascii_is_alphanumeric('.'));
  TEST_CHECK(!ascii_is_alphanumeric(' '));
  TEST_CHECK(!ascii_is_alphanumeric('\0'));
  TEST_CHECK(!ascii_is_alphanumeric(0x7F));
  TEST_CHECK(!ascii_is_alphanumeric(0xC3));
  TEST_CHECK(!ascii_is_alphanumeric(0xFF));
}

// `ascii_to_lower` folds `[A-Z]` and returns every other byte unchanged. The pass-through half has
// no caller-level coverage. A fold that set bit 0x20 unconditionally would make the control byte
// 0x1A compare equal to ':' and 0x0F equal to '/'. The high-byte case is the header's
// locale-independence check: `tolower` from `<ctype.h>` may fold such a byte under some locales,
// and this must not.
static void test_to_lower_folds_uppercase_and_passes_through(void) {
  TEST_CHECK(ascii_to_lower('A') == 'a');
  TEST_CHECK(ascii_to_lower('Z') == 'z');
  TEST_CHECK(ascii_to_lower('M') == 'm');

  // Already lowercase, and the bytes immediately outside `[A-Z]`.
  TEST_CHECK(ascii_to_lower('a') == 'a');
  TEST_CHECK(ascii_to_lower('z') == 'z');
  TEST_CHECK(ascii_to_lower('@') == '@');
  TEST_CHECK(ascii_to_lower('[') == '[');

  // Digits, punctuation and the two control bytes that would alias ':' and '/' under an
  // unconditional fold.
  TEST_CHECK(ascii_to_lower('7') == '7');
  TEST_CHECK(ascii_to_lower(':') == ':');
  TEST_CHECK(ascii_to_lower('/') == '/');
  TEST_CHECK(ascii_to_lower(0x1A) == 0x1A);
  TEST_CHECK(ascii_to_lower(0x0F) == 0x0F);
  TEST_CHECK(ascii_to_lower('\0') == '\0');

  // Above 0x7F nothing is folded, whatever the locale.
  TEST_CHECK(ascii_to_lower(0xC3) == 0xC3);
  TEST_CHECK(ascii_to_lower(0xDF) == 0xDF);
  TEST_CHECK(ascii_to_lower(0xFF) == 0xFF);
}

TEST_LIST = {
    {"is digit accepts only ascii digits", test_is_digit_accepts_only_ascii_digits},
    {"is alnum accepts only letters and digits", test_is_alnum_accepts_only_letters_and_digits},
    {"to lower folds uppercase and passes through",
     test_to_lower_folds_uppercase_and_passes_through},
    {NULL, NULL},
};
