#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/fs.h"
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

// A fixture file lands below the root in nested directories with its text intact, and removing the
// tree deletes the root and everything below it.
static void test_fixture_tree_write_and_remove(void) {
  char root_dir_template[] = "/tmp/cwrap-test-support.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
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
  const int write_rc =
      is_path_complete && mkdir(nested_dir, 0700) == 0 && mkdir(deeper_dir, 0700) == 0
          ? fs_write_file(fixture_path, "fixture text", strlen("fixture text"), NULL, 0)
          : -1;
  FILE* fixture = write_rc == 0 ? fopen(fixture_path, "rb") : NULL;
  const bool is_opened = fixture != NULL;
  char text[64] = "";
  int read_rc = -1;
  int close_rc = -1;
  if (fixture != NULL) {
    read_rc = read_capture(fixture, text, sizeof(text));
    close_rc = fclose(fixture);
  }
  // The tree is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);
  errno = 0;
  const int access_rc = access(root_dir, F_OK);
  const int access_errno = errno;

  TEST_CHECK(root_dir == root_dir_template);
  TEST_CHECK(write_rc == 0);
  TEST_CHECK(is_path_complete);
  TEST_CHECK(is_opened);
  TEST_CHECK(read_rc == 0);
  TEST_CHECK(close_rc == 0);
  TEST_CHECK(strcmp(text, "fixture text") == 0);
  TEST_CHECK(access_rc == -1 && access_errno == ENOENT);
}

// A fixture file holds exactly the `contents_len` bytes passed, including an embedded `NUL`, so the
// helper writes by length rather than stopping at the first terminator. The file is read back with
// `fread` because `fs_read_file` rejects an embedded `NUL`. The open descriptor keeps the contents
// readable after the file is removed.
static void test_init_fixture_file_writes_contents(void) {
  static const char contents[] = "a\0b\n";
  const size_t contents_len = sizeof(contents) - 1;
  char file_path[] = "/tmp/cwrap-test-support.XXXXXX";
  const char* fixture_path = init_fixture_file(file_path, contents, contents_len);
  if (fixture_path == NULL) {
    return;
  }
  TEST_CHECK(fixture_path == file_path);

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

  TEST_CHECK(file_len == contents_len && memcmp(file_data, contents, contents_len) == 0);
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
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(strcmp(text, "captured\n") == 0);
  TEST_CHECK(is_same_file(STDERR_FILENO, &stderr_before));
}

// A stream buffered rather than flushed at the time the capture ends is still captured whole, since
// `capture_end` flushes before restoring. `stdout` is fully buffered when redirected to a file.
static void test_capture_flushes_buffered_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stdout, &capture) == 0);
  TEST_CHECK(fputs("buffered", stdout) >= 0);
  char text[64];
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(strcmp(text, "buffered") == 0);
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
    {"fixture tree write and remove", test_fixture_tree_write_and_remove},
    {"init fixture file writes contents", test_init_fixture_file_writes_contents},
    {"capture reads stream text and restores", test_capture_reads_stream_text_and_restores},
    {"capture flushes buffered text", test_capture_flushes_buffered_text},
    {"capture reads empty text", test_capture_reads_empty_text},
    {"unwritable capture fails writes and discards them",
     test_unwritable_capture_fails_writes_and_discards_them},
    {NULL, NULL},
};
