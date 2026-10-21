#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Reports whether a descriptor refers to the same open file it did when `before` was filled.
 *
 * @param fd     Descriptor to inspect.
 * @param before Stat buffer filled from `fd` before a redirection.
 * @return `true` when `fd` names the same device and inode as `before`, or `false` otherwise.
 */
static bool is_same_file(int fd, const struct stat* before) {
  struct stat after;
  return fstat(fd, &after) == 0 && after.st_dev == before->st_dev && after.st_ino == before->st_ino;
}

/**
 * @brief Runs out of memory. Run through `test_is_oom`.
 *
 * @param userdata Unused.
 */
static void run_out_of_memory(void* userdata) {
  (void)userdata;
  os_oom();
}

/**
 * @brief Returns without running out of memory. Run through `test_is_oom`.
 *
 * @param userdata Unused.
 */
static void return_normally(void* userdata) {
  (void)userdata;
}

// `test_is_oom` reports a child that ends in `os_oom`, and not one that returns.
static void test_detects_only_an_out_of_memory_exit(void) {
  TEST_CHECK(test_is_oom(run_out_of_memory, NULL));
  TEST_CHECK(!test_is_oom(return_normally, NULL));
}

// An `os_oom` that no `test_is_oom` expects exits with status `1`, the one acutest counts as a
// failed test.
static void test_unexpected_oom_exits_with_failure_status(void) {
  (void)fflush(stdout);
  (void)fflush(stderr);
  const pid_t pid = fork();
  TEST_ASSERT(pid >= 0);
  if (pid == 0) {
    // The child's report would read as a failure of this test, so it goes nowhere.
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDERR_FILENO);
    }
    os_oom();
  }
  int status = 0;
  TEST_CHECK(waitpid(pid, &status, 0) == pid);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

// Two generators from one seed draw the same values, and the step is the pinned one: from state
// `0`, the second value is the high half of its multiplier.
static void test_rng_repeats_its_sequence(void) {
  struct TestRng zero = {0};
  TEST_CHECK(test_rng_next(&zero) == 0);
  TEST_CHECK(test_rng_next(&zero) == UINT32_C(0x3243f6a8));
  struct TestRng first = {TEST_RNG_SEED_DEFAULT};
  struct TestRng second = first;
  for (ptrdiff_t i = 0; i < 64; i++) {
    TEST_CHECK(test_rng_next(&first) == test_rng_next(&second));
  }
}

// `test_rng_below` stays below its bound and, over many draws, reaches every value below it.
static void test_rng_below_covers_its_range(void) {
  enum { BOUND = 7, DRAW_COUNT = 1000 };
  struct TestRng rng = {TEST_RNG_SEED_DEFAULT};
  bool is_drawn[BOUND] = {false};
  for (ptrdiff_t i = 0; i < DRAW_COUNT; i++) {
    const ptrdiff_t value = test_rng_below(&rng, BOUND);
    if (!TEST_CHECK(0 <= value && value < BOUND)) {
      return;
    }
    is_drawn[value] = true;
  }
  for (ptrdiff_t i = 0; i < BOUND; i++) {
    TEST_CHECK_(is_drawn[i], "%td is drawn", i);
  }
  TEST_CHECK(test_rng_below(&rng, 1) == 0);
  const ptrdiff_t value = test_rng_below(&rng, UINT32_MAX);
  TEST_CHECK(0 <= value && value < (ptrdiff_t)UINT32_MAX);
}

// `test_printable` keeps printable ASCII and writes every other byte, and the backslash, as `\xNN`.
static void test_printable_escapes_unprintable_bytes(void) {
  const struct Arena region = test_arena();
  struct Arena arena = region;
  const struct Str text = {"a\0b\\ ~\x7F\n\xFF", 9};
  TEST_CHECK(str_eq(test_printable(text, &arena), S("a\\x00b\\x5c ~\\x7f\\x0a\\xff")));
  TEST_CHECK(test_printable(S(""), &arena).len == 0);
  arena_release(region);
}

// A fixture file lands below the root in nested directories with its text intact, and removing the
// tree deletes the root and everything below it.
static void test_fixture_tree_write_and_remove(void) {
  enum { FIXTURE_LEN_MAX = 64 };
  char root_dir[] = "/tmp/cwrap-test-support.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  const struct Str contents = S("fixture text");
  char fixture_path[PATH_MAX];
  const int fixture_path_len =
      snprintf(fixture_path, sizeof(fixture_path), "%s/nested/deeper/file.txt", root_dir);
  const bool is_path_complete =
      fixture_path_len > 0 && (size_t)fixture_path_len < sizeof(fixture_path);
  // Each parent directory path is a prefix of the file path, so it fits when the file path does.
  char nested_dir[PATH_MAX];
  (void)snprintf(nested_dir, sizeof(nested_dir), "%s/nested", root_dir);
  char deeper_dir[PATH_MAX];
  (void)snprintf(deeper_dir, sizeof(deeper_dir), "%s/nested/deeper", root_dir);
  const struct Str fixture = str_from_cstr(fixture_path);
  struct Log log = test_log();
  const int write_rc =
      is_path_complete && mkdir(nested_dir, 0700) == 0 && mkdir(deeper_dir, 0700) == 0
          ? os_write(os_posix(), fixture, contents, S("failed to write fixture"), &log)
          : -1;
  struct Arena arena = test_arena();
  struct Str text = {0};
  const int read_rc = write_rc == 0 ? os_read(os_posix(), fixture, FIXTURE_LEN_MAX, &arena, &text,
                                              S("failed to read fixture"), &log)
                                    : -1;
  // The tree is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);
  errno = 0;
  const int access_rc = access(root_dir, F_OK);
  const int access_errno = errno;

  TEST_CHECK(is_path_complete);
  TEST_CHECK(write_rc == 0);
  TEST_CHECK(read_rc == 0 && test_log_is_empty(&log));
  TEST_CHECK(str_eq(text, contents));
  TEST_CHECK(access_rc == -1 && access_errno == ENOENT);
}

// A fixture file holds exactly the bytes of the string passed, including an embedded `NUL`, so the
// helper writes by length rather than stopping at the first terminator. The file is read back with
// `fread` because `os_read` rejects an embedded `NUL`. The open descriptor keeps the contents
// readable after the file is removed.
static void test_init_fixture_file_writes_contents(void) {
  const struct Str contents = S("a\0b\n");
  char file_path[] = "/tmp/cwrap-test-support.XXXXXX";
  if (init_fixture_file(file_path, contents) != 0) {
    return;
  }

  FILE* file = fopen(file_path, "rb");
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);
  TEST_ASSERT(file != NULL);
  if (file == NULL) {
    return;
  }
  char file_data[16];
  const size_t file_len = fread(file_data, 1, sizeof(file_data), file);
  const bool has_read_failed = ferror(file) != 0;
  const int close_rc = fclose(file);
  TEST_CHECK(!has_read_failed);
  TEST_CHECK(close_rc == 0);

  TEST_CHECK(str_eq((struct Str){file_data, (ptrdiff_t)file_len}, contents));
}

// A capture receives what the stream writes while redirected, and the stream writes to its original
// descriptor again once the capture ends.
static void test_capture_reads_stream_text_and_restores(void) {
  struct stat stderr_before;
  TEST_ASSERT(fstat(STDERR_FILENO, &stderr_before) == 0);

  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stderr, &capture) == 0);
  TEST_CHECK(fputs("captured\n", stderr) >= 0);
  char text[64];
  if (TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0)) {
    TEST_CHECK(strcmp(text, "captured\n") == 0);
  }
  TEST_CHECK(is_same_file(STDERR_FILENO, &stderr_before));
}

// A stream buffered rather than flushed at the time the capture ends is still captured whole, since
// `capture_end` flushes before restoring. `stdout` is fully buffered when redirected to a file.
static void test_capture_flushes_buffered_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stdout, &capture) == 0);
  TEST_CHECK(fputs("buffered", stdout) >= 0);
  char text[64];
  if (TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0)) {
    TEST_CHECK(strcmp(text, "buffered") == 0);
  }
}

// A capture that ends with nothing written reads an empty string.
static void test_capture_reads_empty_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stderr, &capture) == 0);
  char text[8] = "stale";
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(text[0] == '\0');
}

// An unwritable stream fails its flush with `EBADF`, and the text the failure left buffered is
// discarded rather than reaching the restored descriptor. The unwritable capture runs inside an
// ordinary one, so any text that leaked through would land in the outer capture.
static void test_unwritable_capture_fails_writes_and_discards_them(void) {
  struct stat stdout_before;
  TEST_ASSERT(fstat(STDOUT_FILENO, &stdout_before) == 0);

  struct StreamCapture outer;
  TEST_ASSERT(capture_begin(stdout, &outer) == 0);
  struct StreamCapture unwritable;
  TEST_CHECK(capture_begin_unwritable(stdout, &unwritable) == 0);
  TEST_CHECK(fputs("lost", stdout) >= 0);
  errno = 0;
  TEST_CHECK(fflush(stdout) == EOF);
  TEST_CHECK(errno == EBADF);
  TEST_CHECK(capture_end(&unwritable, NULL, 0) == 0);
  TEST_CHECK(ferror(stdout) == 0);
  char text[64];
  TEST_CHECK(capture_end(&outer, text, sizeof(text)) == 0);

  TEST_CHECK(text[0] == '\0');
  TEST_CHECK(is_same_file(STDOUT_FILENO, &stdout_before));
}

TEST_LIST = {
    {"detects only an out of memory exit", test_detects_only_an_out_of_memory_exit},
    {"unexpected oom exits with failure status", test_unexpected_oom_exits_with_failure_status},
    {"rng repeats its sequence", test_rng_repeats_its_sequence},
    {"rng below covers its range", test_rng_below_covers_its_range},
    {"printable escapes unprintable bytes", test_printable_escapes_unprintable_bytes},
    {"fixture tree write and remove", test_fixture_tree_write_and_remove},
    {"init fixture file writes contents", test_init_fixture_file_writes_contents},
    {"capture reads stream text and restores", test_capture_reads_stream_text_and_restores},
    {"capture flushes buffered text", test_capture_flushes_buffered_text},
    {"capture reads empty text", test_capture_reads_empty_text},
    {"unwritable capture fails writes and discards them",
     test_unwritable_capture_fails_writes_and_discards_them},
    {NULL, NULL},
};
