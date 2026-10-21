#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app/wrap_input.h"
#include "os_memory.h"
#include "runtime/memory.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

// These tests run on the in-memory host, so each sets up its files and standard input in the host
// and reads what the input printed back from it. `src/runtime/test_os.c` checks that the host
// behaves as the POSIX one does for every property relied on here.

/** Bytes of the caller-owned input region `test_inputs_reuse_input_region` gives each input. */
enum { REUSED_INPUT_SIZE = 4096 };

/** Bytes of each line `test_inputs_reuse_input_region` writes, a comment already wrapped. */
enum { REUSED_LINE_LEN = 20 };

/** Lines in each file `test_inputs_reuse_input_region` writes. */
enum { REUSED_LINE_COUNT = 40 };

/**
 * Bytes of the caller-owned input region `process_in_tiny_input` gives its input. They hold the
 * 14-byte fixture, but not the rewritten output after it.
 */
enum { TINY_INPUT_SIZE = 24 };

/**
 * @brief Rewrites a fixture source in an input region that holds the source but not its output. Run
 *        through `test_is_oom`, which calls it in a child process.
 *
 * @param userdata Host holding the fixture file `a.c`, as a `struct Os*`. Must not be `NULL`.
 */
static void process_in_tiny_input(void* userdata) {
  struct Os* os = userdata;
  static _Alignas(max_align_t) char memory[TINY_INPUT_SIZE];
  const struct Arena input = {.beg = memory, .end = memory + sizeof(memory)};
  struct Log log = test_log();
  (void)wrap_input_process_file(S("a.c"), 40, false, true, os, input, test_arena(), &log);
}

/** Two fixture files of the same length, for `process_twice_in_one_region`. */
struct ReusedInputs {
  /** Host holding `first.c` and `second.c`. */
  struct Os* os;

  /** Receives the result of processing the first file. */
  enum WrapInputResult first_result;

  /** Receives the result of processing the second file. */
  enum WrapInputResult second_result;

  /** Receives the failures of both. */
  struct Log log;
};

/**
 * @brief Checks both files of `userdata` in turn in one caller-owned input region of
 *        `REUSED_INPUT_SIZE` bytes, passed by value to each.
 *
 * At width 20 every line of the fixtures is already filled, so check mode reports nothing.
 *
 * @param userdata The files, as a `struct ReusedInputs*`, which receives the results. Must not be
 *                 `NULL`.
 */
static void process_twice_in_one_region(void* userdata) {
  struct ReusedInputs* inputs = userdata;
  static _Alignas(max_align_t) char memory[REUSED_INPUT_SIZE];
  const struct Arena input = {.beg = memory, .end = memory + sizeof(memory)};
  inputs->first_result = wrap_input_process_file(S("first.c"), 20, false, true, inputs->os, input,
                                                 test_arena(), &inputs->log);
  inputs->second_result = wrap_input_process_file(S("second.c"), 20, false, true, inputs->os, input,
                                                  test_arena(), &inputs->log);
}

// Stable file input succeeds silently in check mode.
static void test_file_check_ok_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one two\n");
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(test_log_is_empty(&log));
}

// Stable standard input succeeds silently in check mode.
static void test_stdin_check_ok_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one two\n"), 0);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(test_log_is_empty(&log));
}

// File check mode retains the distinct changed result and path attribution, and writes nothing.
static void test_file_check_change_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(test_log_is(&log, "would rewrap 'a.c'\n"));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));
  TEST_CHECK(os_memory_printed(os, 1).len == 0);
}

// Standard input in check mode names it `stdin` when it reports a change.
static void test_stdin_check_change_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n// two\n"), 0);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_CHANGED);
  TEST_CHECK(test_log_is(&log, "would rewrap 'stdin'\n"));
}

// A file in the default mode prints its rewritten source to standard output, and in-place mode
// writes it back to the file instead, printing nothing.
static void test_file_emits_rewritten_source(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  struct Log log = test_log();

  const enum WrapInputResult printed_result =
      wrap_input_process_file(S("a.c"), 40, false, false, os, test_arena(), test_arena(), &log);
  TEST_CHECK(printed_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_printed(os, 1, "// one two\n"));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));

  struct Os* in_place_os = os_memory();
  os_memory_write(in_place_os, "a.c", "// one\n// two\n");
  const enum WrapInputResult written_result = wrap_input_process_file(
      S("a.c"), 40, true, false, in_place_os, test_arena(), test_arena(), &log);
  TEST_CHECK(written_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_file(in_place_os, "a.c", "// one two\n"));
  TEST_CHECK(os_memory_printed(in_place_os, 1).len == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// Two inputs run in turn in the same input region, each from its start: the region holds one of the
// files and its output, but not two. The run that could run out goes through `test_is_oom` first,
// so running out fails a check here.
static void test_inputs_reuse_input_region(void) {
  char contents[REUSED_LINE_LEN * REUSED_LINE_COUNT];
  for (ptrdiff_t i = 0; i < REUSED_LINE_COUNT; i++) {
    memcpy(contents + i * REUSED_LINE_LEN, "// one two three ab\n", REUSED_LINE_LEN);
  }
  const ptrdiff_t contents_len = sizeof(contents);
  struct ReusedInputs inputs = {.os = os_memory(), .log = test_log()};
  os_memory_write_bytes(inputs.os, S("first.c"), (struct Str){contents, contents_len});
  os_memory_write_bytes(inputs.os, S("second.c"), (struct Str){contents, contents_len});
  const bool is_oom = test_is_oom(process_twice_in_one_region, &inputs);
  if (!is_oom) {
    process_twice_in_one_region(&inputs);
  }

  // One input takes the source and an output buffer of up to twice the output, so about three times
  // the file, and two inputs that did not reuse the region would take twice that.
  TEST_CHECK(4 * contents_len < REUSED_INPUT_SIZE && 6 * contents_len > REUSED_INPUT_SIZE);
  TEST_CHECK(!is_oom);
  TEST_CHECK(inputs.first_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(inputs.second_result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(test_log_is_empty(&inputs.log));
}

// An input names its last phase and itself for the out-of-memory report, in the shape of the other
// diagnostics. The phase after the rewrite follows the mode: a check stays in the rewrap, an
// in-place run writes the file, and the default mode writes the rewritten source to `stdout`.
static void test_file_names_phase_for_oom_report(void) {
  static const struct {
    bool is_in_place;
    bool is_check;
    const char* report;
  } cases[] = {
      {false, true, "failed to rewrap file: out of memory ('a.c')\n"},
      {true, false, "failed to write file: out of memory ('a.c')\n"},
      {false, false, "failed to write rewritten file: out of memory ('a.c')\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Os* os = os_memory();
    os_memory_write(os, "a.c", "// one two\n");
    struct Log log = test_log();
    const enum WrapInputResult result =
        wrap_input_process_file(S("a.c"), 40, cases[i].is_in_place, cases[i].is_check, os,
                                test_arena(), test_arena(), &log);
    struct Os* report_os = os_memory();
    memory_oom_report(report_os);
    memory_oom_set_job((struct Str){0}, (struct Str){0}, false);

    TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
    TEST_CHECK(os_memory_is_printed(report_os, 2, cases[i].report));
    TEST_MSG("case %td", i);
  }
}

// Standard input names its write to `stdout` and itself as a stream for the out-of-memory report.
static void test_stdin_names_phase_for_oom_report(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one two\n"), 0);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_stdin(40, false, os, test_arena(), test_arena(), &log);
  struct Os* report_os = os_memory();
  memory_oom_report(report_os);
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);

  TEST_CHECK(result == WRAP_INPUT_RESULT_OK);
  TEST_CHECK(os_memory_is_printed(
      report_os, 2, "failed to write rewritten input: out of memory (from 'stdin')\n"));
}

// A file read error remains distinct from a check-mode change and keeps the existing diagnostic,
// for a missing file and for a read that fails after the open.
static void test_file_read_error_result(void) {
  struct Os* os = os_memory();
  struct Log log = test_log();
  const enum WrapInputResult missing_result = wrap_input_process_file(
      S("missing.c"), 40, false, true, os, test_arena(), test_arena(), &log);
  os_memory_write(os, "a.c", "// one\n");
  os_memory_fail(os, OS_MEMORY_OP_READ, "a.c", EIO);
  const enum WrapInputResult failed_result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(missing_result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(failed_result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log,
                         "failed to read file: %s ('missing.c')\n"
                         "failed to read file: %s ('a.c')\n",
                         strerror(ENOENT), strerror(EIO)));
}

// A source file over `SOURCE_LEN_MAX` is rejected from its size before it is read, naming the limit
// and the size. The file is extended as `truncate` would, so the host holds only its first byte.
static void test_file_oversize_error_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "a");
  const ptrdiff_t oversize_len = (ptrdiff_t)SOURCE_LEN_MAX + 1;
  os_memory_truncate(os, "a.c", oversize_len);

  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, false, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log,
                         "failed to read file: exceeds max file size (%d bytes) at %td bytes "
                         "('a.c')\n",
                         SOURCE_LEN_MAX, oversize_len));
}

// A standard input read failure is attributed to `stdin` and remains an operational error.
static void test_stdin_read_error_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n"), EIO);
  struct Log log = test_log();

  const enum WrapInputResult result =
      wrap_input_process_stdin(40, true, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to read input: %s (from 'stdin')\n", strerror(EIO)));
}

// A file whose output cannot be written to standard output fails with the write's own reason,
// naming the file, whether the stream fails at once or after a short write.
static void test_file_stdout_error_result(void) {
  static const int error_numbers[] = {EPIPE, EIO};
  static const ptrdiff_t accepted_lens[] = {0, 4};
  for (ptrdiff_t i = 0; i < 2; i++) {
    struct Os* os = os_memory();
    os_memory_write(os, "a.c", "// one\n// two\n");
    os_memory_fail_print(os, 1, accepted_lens[i], error_numbers[i]);
    struct Log log = test_log();
    const enum WrapInputResult result =
        wrap_input_process_file(S("a.c"), 40, false, false, os, test_arena(), test_arena(), &log);

    TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
    TEST_CHECK(test_log_is(&log, "failed to write rewritten file to 'stdout': %s (for 'a.c')\n",
                           strerror(error_numbers[i])));
    TEST_CHECK(os_memory_printed(os, 1).len == accepted_lens[i]);
  }
}

// An in-place write that fails leaves the file as it was and reports the write's own reason.
static void test_file_in_place_write_error_result(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  os_memory_fail(os, OS_MEMORY_OP_WRITE, "a.c", ENOSPC);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_file(S("a.c"), 40, true, false, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('a.c')\n", strerror(ENOSPC)));
  TEST_CHECK(os_memory_is_file(os, "a.c", "// one\n// two\n"));
}

// Standard input whose output cannot be written fails the same way, naming the stream.
static void test_stdin_stdout_error_result(void) {
  struct Os* os = os_memory();
  os_memory_stdin(os, S("// one\n"), 0);
  os_memory_fail_print(os, 1, 0, EPIPE);
  struct Log log = test_log();
  const enum WrapInputResult result =
      wrap_input_process_stdin(40, false, os, test_arena(), test_arena(), &log);

  TEST_CHECK(result == WRAP_INPUT_RESULT_ERROR);
  TEST_CHECK(test_log_is(&log, "failed to write rewritten input to 'stdout': %s (from 'stdin')\n",
                         strerror(EPIPE)));
}

// An input whose region is too small ends in `os_oom` rather than failing with a diagnostic or
// overrunning the region. Under the test build's `os_oom` that is the process exiting with
// `TEST_OOM_EXIT_STATUS`.
static void test_tiny_input_region_ends_in_oom(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "a.c", "// one\n// two\n");
  TEST_CHECK(test_is_oom(process_in_tiny_input, os));
}

TEST_LIST = {
    {"file check ok result", test_file_check_ok_result},
    {"stdin check ok result", test_stdin_check_ok_result},
    {"file check change result", test_file_check_change_result},
    {"stdin check change result", test_stdin_check_change_result},
    {"file emits rewritten source", test_file_emits_rewritten_source},
    {"inputs reuse input region", test_inputs_reuse_input_region},
    {"file names phase for oom report", test_file_names_phase_for_oom_report},
    {"stdin names phase for oom report", test_stdin_names_phase_for_oom_report},
    {"file read error result", test_file_read_error_result},
    {"file oversize error result", test_file_oversize_error_result},
    {"stdin read error result", test_stdin_read_error_result},
    {"file stdout error result", test_file_stdout_error_result},
    {"file in place write error result", test_file_in_place_write_error_result},
    {"stdin stdout error result", test_stdin_stdout_error_result},
    {"tiny input region ends in oom", test_tiny_input_region_ends_in_oom},
    {NULL, NULL},
};
