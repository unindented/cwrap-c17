#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app/cmd_wrap.h"
#include "app/exit_code.h"
#include "app/wrap_input.h"
#include "os_memory.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/str.h"
#include "test_support.h"

// These tests run on the in-memory host, so each sets up its files and standard input in the host
// and reads back what the run printed and wrote. `src/runtime/test_os.c` checks that the host
// behaves as the POSIX one does for every property relied on here.

/** Bytes of the source `test_wraps_large_file` wraps, at least a mebibyte. */
enum { SOURCE_LEN_HUGE = 2 * 1024 * 1024 };

/** Columns of the indent before the comment `test_wraps_deep_indent_at_width_one` wraps. */
enum { DEEP_INDENT_COLUMNS = 200 };

/** One-letter words in the comment `test_wraps_deep_indent_at_width_one` wraps. */
enum { DEEP_WORD_COUNT = 40000 };

/** Source the wrap-command tests start from, which rewraps to one line at width 40. */
static const char wrap_fixture_source[] = "// one\n// two\n";

/** Number of paths `bad_inputs` adds. */
enum { BAD_INPUT_COUNT = 5 };

/**
 * Paths among which three are bad, for the tests that no input is written when one is bad. The good
 * ones need rewrapping, so a write would show. The bad ones come in path order `nul`, `missing`,
 * `dir`.
 */
static char* bad_input_paths[BAD_INPUT_COUNT] = {"first.c", "nul.c", "missing.c", "last.c", "dir"};

/** Messages an in-place run reports for `bad_input_paths`, with the `ENOENT` reason as `%s`. */
#define BAD_INPUT_MESSAGES                                         \
  "failed to read file: contains an embedded NUL byte ('nul.c')\n" \
  "failed to read file: %s ('missing.c')\n"                        \
  "failed to read file: not a regular file ('dir')\n"

/**
 * @brief Returns a host holding the files `bad_input_paths` names: two that need rewrapping, one
 *        holding a `NUL`, and a directory. `missing.c` does not exist.
 *
 * @return The host, never `NULL`.
 */
static struct Os* bad_inputs(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "first.c", wrap_fixture_source);
  os_memory_write_bytes(os, S("nul.c"), S("// one\0\n"));
  os_memory_write(os, "last.c", wrap_fixture_source);
  os_memory_mkdir(os, "dir");
  return os;
}

/**
 * @brief Returns the options of a file-mode run over `paths` at width 40.
 *
 * @param paths      Paths of the run. Must not be `NULL`.
 * @param path_count Number of paths.
 * @return The options, in the default mode.
 */
static struct WrapOptions file_options(char** paths, ptrdiff_t path_count) {
  return (struct WrapOptions){.width = 40, .paths = paths, .path_count = path_count};
}

// A readable source is rewritten to standard output, and the command reports success without a
// diagnostic.
static void test_prints_rewritten_source(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  char* paths[] = {"a.c"};
  const struct WrapOptions options = file_options(paths, 1);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(os_memory_printed(os, 2).len == 0);
}

// Check mode reports a source that would change, prints nothing to standard output, and leaves the
// source untouched.
static void test_reports_check_change(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  char* paths[] = {"a.c"};
  struct WrapOptions options = file_options(paths, 1);
  options.is_check = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  TEST_CHECK(os_memory_is_printed(os, 2, "would rewrap 'a.c'\n"));
  TEST_CHECK(os_memory_is_file(os, "a.c", wrap_fixture_source));
}

// In-place mode overwrites a changed source, prints nothing to either stream, and reports success.
static void test_rewrites_in_place(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  char* paths[] = {"a.c"};
  struct WrapOptions options = file_options(paths, 1);
  options.is_in_place = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  TEST_CHECK(os_memory_printed(os, 2).len == 0);
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one two\n"));
}

// One file given twice in place succeeds and holds the bytes of a single rewrite, because the
// second pass finds it already wrapped.
static void test_in_place_accepts_file_given_twice(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  char* paths[] = {"a.c", "a.c"};
  struct WrapOptions options = file_options(paths, 2);
  options.is_in_place = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_printed(os, 2).len == 0);
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one two\n"));
}

// A comment indented far past a narrow width, which writes a hundred times its size because every
// word gets a line of its own behind a copy of the indent, is rewritten in place. Regions sized
// only as a multiple of the source would run out of memory on it.
static void test_wraps_deep_indent_at_width_one(void) {
  enum { SOURCE_LEN = DEEP_INDENT_COLUMNS + 2 + 2 * DEEP_WORD_COUNT + 1 };
  struct Arena arena = test_arena();
  char* source = arena_new(&arena, SOURCE_LEN, char);
  memset(source, ' ', DEEP_INDENT_COLUMNS);
  memcpy(source + DEEP_INDENT_COLUMNS, "//", 2);
  for (ptrdiff_t i = 0; i < DEEP_WORD_COUNT; i++) {
    memcpy(source + DEEP_INDENT_COLUMNS + 2 + 2 * i, " a", 2);
  }
  source[SOURCE_LEN - 1] = '\n';
  struct Os* os = os_memory();
  os_memory_write_bytes(os, S("a.c"), (struct Str){source, SOURCE_LEN});
  char* paths[] = {"a.c"};
  struct WrapOptions options = file_options(paths, 1);
  options.width = 1;
  options.is_in_place = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(os_memory_printed(os, 2).len == 0);
  // Each word is a line of the indent, the marker, a space, the word and a newline.
  struct Buf expected = {.arena = &arena};
  for (ptrdiff_t i = 0; i < DEEP_WORD_COUNT; i++) {
    buf_append(&expected, (struct Str){source, DEEP_INDENT_COLUMNS + 2});
    buf_append(&expected, S(" a\n"));
  }
  TEST_CHECK(os_memory_is_file(os, "a.c", buf_cstr(&expected)));
}

// A source of about two mebibytes is wrapped within the reserved regions. Each comment is one line
// already wrapped, ended by a line of code, so the output is the source.
static void test_wraps_large_file(void) {
  static const char line[] =
      "// alpha beta gamma delta epsilon zeta eta theta iota kappa\nint x;\n";
  const ptrdiff_t line_len = (ptrdiff_t)sizeof(line) - 1;
  struct Arena arena = test_arena();
  struct Buf source = {.arena = &arena};
  while (source.len + line_len <= SOURCE_LEN_HUGE) {
    buf_append(&source, (struct Str){line, line_len});
  }
  struct Os* os = os_memory();
  os_memory_write_bytes(os, S("a.c"), buf_str(&source));
  char* paths[] = {"a.c"};
  struct WrapOptions options = file_options(paths, 1);
  options.width = 100;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_OK);
  TEST_CHECK(str_eq(os_memory_printed(os, 1), buf_str(&source)));
  TEST_CHECK(os_memory_printed(os, 2).len == 0);
}

// A missing input reports the read failure on standard error, prints nothing to standard output,
// and returns the runtime failure code.
static void test_reports_missing_file(void) {
  struct Os* os = os_memory();
  char* paths[] = {"missing.c"};
  const struct WrapOptions options = file_options(paths, 1);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), "failed to read file: %s ('missing.c')\n",
                          strerror(ENOENT)));
}

// Two inputs that both fail in check mode log one line each, so neither is lost to the other. This
// is the postcondition `cmd_wrap_run` states for check mode: a log that overwrote, or that dropped
// the newline and ran two messages together, would still satisfy every single-file test.
static void test_reports_one_line_per_failing_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  char* paths[] = {"missing.c", "a.c"};
  struct WrapOptions options = file_options(paths, 2);
  options.is_check = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  // Paths are reported in argument order. Compared whole, with the separator in the middle, so a
  // dropped newline or a lost line both fail here.
  TEST_CHECK(test_text_is(os_memory_printed(os, 2),
                          "failed to read file: %s ('missing.c')\nwould rewrap 'a.c'\n",
                          strerror(ENOENT)));
}

// A source that rewrites fine but cannot be printed fails with the write's own reason, naming the
// file, rather than exiting 0 when nobody received the output. The reason it reports is what lets
// the user tell a closed pipe from a full disk. The run stops at the first input whose output is
// lost, so the input after it is not printed either.
static void test_reports_unwritable_stdout(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  os_memory_write(os, "b.c", wrap_fixture_source);
  os_memory_fail_print(os, 1, 0, EPIPE);
  char* paths[] = {"a.c", "b.c"};
  const struct WrapOptions options = file_options(paths, 2);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(test_text_is(os_memory_printed(os, 2),
                          "failed to write rewritten file to 'stdout': %s (for 'a.c')\n",
                          strerror(EPIPE)));
}

// An in-place write that fails stops the run with the write's own reason and leaves the file as it
// was, so a full disk never truncates a user's source.
static void test_reports_failed_in_place_write(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  os_memory_write(os, "b.c", wrap_fixture_source);
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "a.c", ENOSPC);
  char* paths[] = {"a.c", "b.c"};
  struct WrapOptions options = file_options(paths, 2);
  options.is_in_place = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), "failed to write file: %s ('a.c')\n",
                          strerror(ENOSPC)));
  TEST_CHECK(os_memory_is_file(os, "a.c", wrap_fixture_source));
  TEST_CHECK(os_memory_is_file(os, "b.c", wrap_fixture_source));
}

// A path the process may not open is reported with the system's reason. The default mode prints the
// file before it and stops there.
static void test_reports_unreadable_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  os_memory_write(os, "locked.c", wrap_fixture_source);
  os_memory_fail(os, OS_MEMORY_OP_OPEN, "locked.c", EACCES);
  char* paths[] = {"a.c", "locked.c"};
  const struct WrapOptions options = file_options(paths, 2);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), "failed to read file: %s ('locked.c')\n",
                          strerror(EACCES)));
}

// A file over `SOURCE_LEN_MAX` is refused from its size, after the default mode printed the file
// before it. The file is extended as `truncate` would, so the host holds only its first byte.
static void test_reports_oversize_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", wrap_fixture_source);
  os_memory_write(os, "big.c", "a");
  const ptrdiff_t oversize_len = (ptrdiff_t)SOURCE_LEN_MAX + 1;
  os_memory_truncate(os, "big.c", oversize_len);
  char* paths[] = {"a.c", "big.c"};
  const struct WrapOptions options = file_options(paths, 2);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(test_text_is(os_memory_printed(os, 2),
                          "failed to read file: exceeds max file size (%d bytes) at %td bytes "
                          "('big.c')\n",
                          SOURCE_LEN_MAX, oversize_len));
}

// The default mode prints each input as it goes and stops at the first bad one, the file holding a
// `NUL`, after printing the file before it.
static void test_default_mode_stops_at_first_bad_input(void) {
  struct Os* os = bad_inputs();
  const struct WrapOptions options = file_options(bad_input_paths, BAD_INPUT_COUNT);

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(os_memory_is_printed(
      os, 2, "failed to read file: contains an embedded NUL byte ('nul.c')\n"));
}

// In-place mode reads every input before it writes any, reports each bad one in path order, and
// rewrites none of the files, not even the one before the first bad path.
static void test_in_place_writes_nothing_when_an_input_is_bad(void) {
  struct Os* os = bad_inputs();
  struct WrapOptions options = file_options(bad_input_paths, BAD_INPUT_COUNT);
  options.is_in_place = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  TEST_CHECK(test_text_is(os_memory_printed(os, 2), BAD_INPUT_MESSAGES, strerror(ENOENT)));
  TEST_CHECK(os_memory_is_file(os, "first.c", wrap_fixture_source));
  TEST_CHECK(os_memory_is_file(os, "last.c", wrap_fixture_source));
}

// Check mode writes nothing anyway, so it reads each input once and reports the same inputs in path
// order, each bad path among the files that would change.
static void test_check_mode_reports_bad_inputs_in_path_order(void) {
  struct Os* os = bad_inputs();
  struct WrapOptions options = file_options(bad_input_paths, BAD_INPUT_COUNT);
  options.is_check = true;

  TEST_CHECK(cmd_wrap_run(&options, os) == EXIT_CODE_FAILURE);
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
  TEST_CHECK(test_text_is(os_memory_printed(os, 2),
                          "would rewrap 'first.c'\n"
                          "failed to read file: contains an embedded NUL byte ('nul.c')\n"
                          "failed to read file: %s ('missing.c')\n"
                          "would rewrap 'last.c'\n"
                          "failed to read file: not a regular file ('dir')\n",
                          strerror(ENOENT)));
}

TEST_LIST = {
    {"prints rewritten source", test_prints_rewritten_source},
    {"reports check change", test_reports_check_change},
    {"rewrites in place", test_rewrites_in_place},
    {"in place accepts file given twice", test_in_place_accepts_file_given_twice},
    {"wraps deep indent at width one", test_wraps_deep_indent_at_width_one},
    {"wraps large file", test_wraps_large_file},
    {"reports missing file", test_reports_missing_file},
    {"reports one line per failing file", test_reports_one_line_per_failing_file},
    {"reports unwritable stdout", test_reports_unwritable_stdout},
    {"reports failed in place write", test_reports_failed_in_place_write},
    {"reports unreadable file", test_reports_unreadable_file},
    {"reports oversize file", test_reports_oversize_file},
    {"default mode stops at first bad input", test_default_mode_stops_at_first_bad_input},
    {"in place writes nothing when an input is bad",
     test_in_place_writes_nothing_when_an_input_is_bad},
    {"check mode reports bad inputs in path order",
     test_check_mode_reports_bad_inputs_in_path_order},
    {NULL, NULL},
};
