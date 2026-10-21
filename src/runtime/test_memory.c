#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "os_memory.h"
#include "runtime/memory.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Bytes of the buffer the discard tests give back, a whole number of pages on every system. */
enum { TEST_DISCARD_LEN = 256 * 1024 };

/**
 * @brief Reserves a size still over the reservation limit after every halving. A `test_is_oom`
 *        callback.
 *
 * @param userdata Unused.
 */
static void reserve_refused(void* userdata) {
  (void)userdata;
  arena_reserve_len_max = TEST_ARENA_LEN;
  struct Arena block = {0};
  (void)arena_reserve_halving((ptrdiff_t)TEST_ARENA_LEN << (ARENA_RESERVE_HALVING_COUNT_MAX + 1),
                              &block);
}

// A reservation the system grants keeps its full size.
static void test_reserve_keeps_granted_size(void) {
  struct Arena block = {0};
  TEST_CHECK(arena_reserve_halving(TEST_ARENA_LEN, &block) == 0);
  TEST_CHECK(block.limit - block.beg == TEST_ARENA_LEN);
  arena_release(block);
}

// Under a limit, each refused size is halved, and the first size that fits is halved once more:
// four and two times the limit are refused, the limit fits, and half of it is reserved.
static void test_reserve_halves_refused_size(void) {
  arena_reserve_len_max = TEST_ARENA_LEN;
  struct Arena block = {0};
  TEST_CHECK(arena_reserve_halving(4 * TEST_ARENA_LEN, &block) == 3);
  TEST_CHECK(block.limit - block.beg == TEST_ARENA_LEN / 2);
  arena_release(block);
  arena_reserve_len_max = PTRDIFF_MAX;
}

// A size still over the limit after every halving ends in the out-of-memory report.
static void test_reserve_refused_after_halving_is_oom(void) {
  TEST_CHECK(test_is_oom(reserve_refused, NULL));
}

// A discarded buffer reads as zero and stays writable, and the bytes on either side of it keep
// their values.
static void test_discard_zeroes_buffer_only(void) {
  struct Arena arena = test_arena();
  char* before = arena_new(&arena, 1, char);
  char* buffer = arena_new(&arena, TEST_DISCARD_LEN, char);
  char* after = arena_new(&arena, 1, char);
  *before = 'a';
  memset(buffer, 'b', TEST_DISCARD_LEN);
  *after = 'c';
  arena_discard(&arena, buffer, TEST_DISCARD_LEN);
  TEST_CHECK(buffer[TEST_DISCARD_LEN / 2] == 0);
  TEST_CHECK(*before == 'a');
  TEST_CHECK(*after == 'c');
  buffer[TEST_DISCARD_LEN / 2] = 'd';
  TEST_CHECK(buffer[TEST_DISCARD_LEN / 2] == 'd');
}

// An arena over memory the caller owns is left alone, because the pages around it are not the
// arena's to map.
static void test_discard_leaves_caller_memory(void) {
  static char storage[TEST_DISCARD_LEN];
  memset(storage, 'x', sizeof(storage));
  const struct Arena arena = {.beg = storage, .end = storage + sizeof(storage)};
  arena_discard(&arena, storage, (ptrdiff_t)sizeof(storage));
  TEST_CHECK(storage[TEST_DISCARD_LEN / 2] == 'x');
}

// The report names the phase and the file in the shape of the other diagnostics.
static void test_oom_report_names_file(void) {
  memory_oom_set_job(S("rewrap"), S("src/a.c"), false);
  struct Os* os = os_memory();
  memory_oom_report(os);
  TEST_CHECK(os_memory_is_printed(os, 2, "failed to rewrap file: out of memory ('src/a.c')\n"));
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);
}

// A file named by a view into a longer string is printed up to the view's length only.
static void test_oom_report_names_file_view(void) {
  memory_oom_set_job(S("rewrap"), str_head(S("src/a.c.tmp"), S("src/a.c").len), false);
  struct Os* os = os_memory();
  memory_oom_report(os);
  TEST_CHECK(os_memory_is_printed(os, 2, "failed to rewrap file: out of memory ('src/a.c')\n"));
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);
}

// A stream is named as the input it came from, in the `(from '<name>')` shape.
static void test_oom_report_names_stream(void) {
  memory_oom_set_job(S("read"), S("stdin"), true);
  struct Os* os = os_memory();
  memory_oom_report(os);
  TEST_CHECK(os_memory_is_printed(os, 2, "failed to read input: out of memory (from 'stdin')\n"));
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);
}

// The report prints the command's log, then the line naming the input it ran out in, so the errors
// found before memory ran out are not lost.
static void test_oom_report_prints_log_and_file(void) {
  struct Log log = test_log();
  (void)log_error(&log, "would rewrap 'a.c'");
  (void)log_error(&log, "would rewrap 'b.c'");
  memory_oom_set_log(&log);
  memory_oom_set_job(S("rewrap"), S("c.c"), false);
  struct Os* os = os_memory();
  memory_oom_report(os);
  TEST_CHECK(os_memory_is_printed(os, 2,
                                  "would rewrap 'a.c'\nwould rewrap 'b.c'\n"
                                  "failed to rewrap file: out of memory ('c.c')\n"));
  memory_oom_set_log(NULL);
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);
}

// With no log and no input named, the report is the bare line.
static void test_oom_report_without_context_is_one_line(void) {
  struct Os* os = os_memory();
  memory_oom_report(os);
  TEST_CHECK(os_memory_is_printed(os, 2, "failed to run: out of memory\n"));
}

TEST_LIST = {
    {"reserve keeps granted size", test_reserve_keeps_granted_size},
    {"reserve halves refused size", test_reserve_halves_refused_size},
    {"reserve refused after halving is oom", test_reserve_refused_after_halving_is_oom},
    {"discard zeroes buffer only", test_discard_zeroes_buffer_only},
    {"discard leaves caller memory", test_discard_leaves_caller_memory},
    {"oom report names file", test_oom_report_names_file},
    {"oom report names file view", test_oom_report_names_file_view},
    {"oom report names stream", test_oom_report_names_stream},
    {"oom report prints log and file", test_oom_report_prints_log_and_file},
    {"oom report without context is one line", test_oom_report_without_context_is_one_line},
    {NULL, NULL},
};
