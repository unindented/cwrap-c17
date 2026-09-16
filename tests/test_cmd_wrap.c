#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/cmd_wrap.h"
#include "app/exit_code.h"
#include "core/error.h"
#include "runtime/fs.h"

/**
 * @brief Writes text to a unique temporary file.
 *
 * @param contents Terminated text to write.
 * @return An allocated path the caller must unlink and free, or `NULL` on test-plumbing failure.
 */
static char* write_temp_file(const char* contents) {
  char file_path[] = "/tmp/cwrap-cmd-wrap.XXXXXX";
  const int fd = mkstemp(file_path);
  TEST_ASSERT(fd >= 0);
  if (fd < 0) {
    return NULL;
  }
  const int close_rc = close(fd);
  TEST_CHECK(close_rc == 0);
  if (close_rc != 0) {
    (void)unlink(file_path);
    return NULL;
  }

  char* file_path_out = malloc(sizeof(file_path));
  TEST_ASSERT(file_path_out != NULL);
  if (file_path_out == NULL) {
    (void)unlink(file_path);
    return NULL;
  }
  memcpy(file_path_out, file_path, sizeof(file_path));
  const int write_rc = fs_write_file(file_path_out, contents, strlen(contents), NULL, 0);
  TEST_CHECK(write_rc == 0);
  if (write_rc != 0) {
    (void)unlink(file_path_out);
    free(file_path_out);
    return NULL;
  }
  return file_path_out;
}

/**
 * @brief Reads an anonymous capture stream.
 *
 * @param capture      Stream to rewind and read. Must not be `NULL`.
 * @param text_out     Buffer that receives terminated captured text.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int read_capture(FILE* capture, char* text_out, size_t text_out_len) {
  TEST_ASSERT(text_out_len > 0);
  if (fseek(capture, 0, SEEK_SET) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  const size_t text_len = fread(text_out, 1, text_out_len - 1, capture);
  text_out[text_len] = '\0';
  if (text_len == text_out_len - 1) {
    const int trailing = fgetc(capture);
    TEST_CHECK(trailing == EOF);
    if (trailing != EOF) {
      return -1;
    }
  }
  if (ferror(capture) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

/**
 * @brief Runs the wrap command while capturing both standard streams.
 *
 * @param options        Wrap options passed to `cmd_wrap_run`.
 * @param stdout_out     Buffer that receives terminated standard output.
 * @param stdout_out_len Size of `stdout_out` in bytes. Must be non-zero.
 * @param stderr_out     Buffer that receives terminated standard error.
 * @param stderr_out_len Size of `stderr_out` in bytes. Must be non-zero.
 * @return The command exit code, or `EXIT_CODE_VALUE_MAX` on test-plumbing failure.
 */
static enum ExitCode run_wrap_capturing(const struct WrapOptions* options,
                                        char* stdout_out,
                                        size_t stdout_out_len,
                                        char* stderr_out,
                                        size_t stderr_out_len) {
  stdout_out[0] = '\0';
  stderr_out[0] = '\0';
  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  FILE* stdout_capture = NULL;
  FILE* stderr_capture = NULL;

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    return rc;
  }

  saved_stdout = dup(STDOUT_FILENO);
  TEST_CHECK(saved_stdout >= 0);
  if (saved_stdout < 0) {
    goto cleanup;
  }
  saved_stderr = dup(STDERR_FILENO);
  TEST_CHECK(saved_stderr >= 0);
  if (saved_stderr < 0) {
    goto cleanup;
  }
  stdout_capture = tmpfile();
  TEST_ASSERT(stdout_capture != NULL);
  if (stdout_capture == NULL) {
    goto cleanup;
  }
  stderr_capture = tmpfile();
  TEST_ASSERT(stderr_capture != NULL);
  if (stderr_capture == NULL) {
    goto cleanup;
  }

  {
    const int stdout_redirect_rc = dup2(fileno(stdout_capture), STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cmd_wrap_run(options);
    const int captured_stdout_flush_rc = fflush(stdout);
    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stdout_flush_rc == 0);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stdout_flush_rc != 0 || captured_stderr_flush_rc != 0;
  }

cleanup:
  if (saved_stdout >= 0) {
    const int restore_rc = dup2(saved_stdout, STDOUT_FILENO);
    TEST_CHECK(restore_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDOUT_FILENO;
    const int close_rc = close(saved_stdout);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (saved_stderr >= 0) {
    const int restore_rc = dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(restore_rc == STDERR_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDERR_FILENO;
    const int close_rc = close(saved_stderr);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  clearerr(stdout);
  clearerr(stderr);

  if (stdout_capture != NULL) {
    has_plumbing_failed =
        read_capture(stdout_capture, stdout_out, stdout_out_len) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stdout_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (stderr_capture != NULL) {
    has_plumbing_failed =
        read_capture(stderr_capture, stderr_out, stderr_out_len) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  return has_plumbing_failed ? (enum ExitCode)EXIT_CODE_VALUE_MAX : rc;
}

// A readable source is rewritten to `stdout`, and the command reports success without a diagnostic.
static void test_prints_rewritten_source(void) {
  char* file_path = write_temp_file("// one\n// two\n");
  if (file_path == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  TEST_CHECK(strcmp(stdout_out, "// one two\n") == 0);
  TEST_CHECK(stderr_out[0] == '\0');

  (void)unlink(file_path);
  free(file_path);
}

// Check mode reports a source that would change, writes nothing to `stdout`, and leaves the source
// untouched.
static void test_reports_check_change(void) {
  char* file_path = write_temp_file("// one\n// two\n");
  if (file_path == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
      .is_check = true,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected), "would rewrap '%s'\n", file_path);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "// one\n// two\n") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
  free(file_path);
}

// In-place mode overwrites a changed source, writes nothing to either stream, and reports success.
static void test_rewrites_in_place(void) {
  char* file_path = write_temp_file("// one\n// two\n");
  if (file_path == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
      .is_in_place = true,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_OK);
  TEST_CHECK(stdout_out[0] == '\0');
  TEST_CHECK(stderr_out[0] == '\0');

  char* file_data = NULL;
  size_t file_len = 0;
  TEST_CHECK(fs_read_file(file_path, &file_data, &file_len, NULL, 0) == 0);
  TEST_ASSERT(file_data != NULL);
  if (file_data != NULL) {
    TEST_CHECK(strcmp(file_data, "// one two\n") == 0);
    free(file_data);
  }
  (void)unlink(file_path);
  free(file_path);
}

// A missing input reports the read failure to `stderr`, writes nothing to `stdout`, and returns the
// runtime failure code.
static void test_reports_missing_file(void) {
  char* missing_path = write_temp_file("");
  if (missing_path == NULL) {
    return;
  }
  (void)unlink(missing_path);
  char* paths[] = {missing_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  char stdout_out[1024];
  char stderr_out[1024];
  const enum ExitCode rc =
      run_wrap_capturing(&options, stdout_out, sizeof(stdout_out), stderr_out, sizeof(stderr_out));
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  TEST_CHECK(stdout_out[0] == '\0');
  char reason[FS_REASON_SIZE];
  char expected[ERROR_MESSAGE_SIZE];
  const int n = snprintf(expected, sizeof(expected), "failed to read file: %s ('%s')\n",
                         error_system_message(reason, sizeof(reason), ENOENT), missing_path);
  TEST_ASSERT(n > 0 && (size_t)n < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  free(missing_path);
}

// A source that rewrites successfully but cannot be written out fails with the stream's own reason
// instead of returning success when nobody received the output. A read-only `/dev/null` on
// `STDOUT_FILENO` deterministically reaches the buffered flush failure. This test performs its own
// redirection because `run_wrap_capturing` needs a writable `stdout` capture.
static void test_reports_unwritable_stdout(void) {
  char* file_path = write_temp_file("// one\n// two\n");
  if (file_path == NULL) {
    return;
  }
  char* paths[] = {file_path};
  const struct WrapOptions options = {
      .width = 40,
      .paths = paths,
      .path_count = 1,
  };

  enum ExitCode rc = (enum ExitCode)EXIT_CODE_VALUE_MAX;
  bool has_plumbing_failed = false;
  int saved_stdout = -1;
  int saved_stderr = -1;
  int unwritable = -1;
  int sink = -1;
  FILE* stderr_capture = NULL;
  char stderr_out[1024] = "";

  const int stdout_flush_rc = fflush(stdout);
  const int stderr_flush_rc = fflush(stderr);
  TEST_CHECK(stdout_flush_rc == 0);
  TEST_CHECK(stderr_flush_rc == 0);
  if (stdout_flush_rc != 0 || stderr_flush_rc != 0) {
    clearerr(stdout);
    clearerr(stderr);
    goto cleanup;
  }

  saved_stdout = dup(STDOUT_FILENO);
  TEST_CHECK(saved_stdout >= 0);
  if (saved_stdout < 0) {
    goto cleanup;
  }
  saved_stderr = dup(STDERR_FILENO);
  TEST_CHECK(saved_stderr >= 0);
  if (saved_stderr < 0) {
    goto cleanup;
  }
  unwritable = open("/dev/null", O_RDONLY);
  TEST_CHECK(unwritable >= 0);
  if (unwritable < 0) {
    goto cleanup;
  }
  sink = open("/dev/null", O_WRONLY);
  TEST_CHECK(sink >= 0);
  if (sink < 0) {
    goto cleanup;
  }
  stderr_capture = tmpfile();
  TEST_ASSERT(stderr_capture != NULL);
  if (stderr_capture == NULL) {
    goto cleanup;
  }

  {
    const int stdout_redirect_rc = dup2(unwritable, STDOUT_FILENO);
    TEST_CHECK(stdout_redirect_rc == STDOUT_FILENO);
    if (stdout_redirect_rc != STDOUT_FILENO) {
      goto cleanup;
    }
    const int stderr_redirect_rc = dup2(fileno(stderr_capture), STDERR_FILENO);
    TEST_CHECK(stderr_redirect_rc == STDERR_FILENO);
    if (stderr_redirect_rc != STDERR_FILENO) {
      goto cleanup;
    }

    rc = cmd_wrap_run(&options);

    const int captured_stderr_flush_rc = fflush(stderr);
    TEST_CHECK(captured_stderr_flush_rc == 0);
    has_plumbing_failed = captured_stderr_flush_rc != 0;
    // The failed write left the rewritten source in `stdout`'s buffer. Drain it into a writable
    // sink before restoring the real descriptor, or the next successful flush prints it to the
    // harness.
    const int sink_redirect_rc = dup2(sink, STDOUT_FILENO);
    TEST_CHECK(sink_redirect_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || sink_redirect_rc != STDOUT_FILENO;
    if (sink_redirect_rc == STDOUT_FILENO) {
      clearerr(stdout);
      const int drain_rc = fflush(stdout);
      TEST_CHECK(drain_rc == 0);
      has_plumbing_failed = has_plumbing_failed || drain_rc != 0;
    }
  }

cleanup:
  if (saved_stdout >= 0) {
    const int restore_rc = dup2(saved_stdout, STDOUT_FILENO);
    TEST_CHECK(restore_rc == STDOUT_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDOUT_FILENO;
    const int close_rc = close(saved_stdout);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (saved_stderr >= 0) {
    const int restore_rc = dup2(saved_stderr, STDERR_FILENO);
    TEST_CHECK(restore_rc == STDERR_FILENO);
    has_plumbing_failed = has_plumbing_failed || restore_rc != STDERR_FILENO;
    const int close_rc = close(saved_stderr);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  clearerr(stdout);
  clearerr(stderr);
  if (unwritable >= 0) {
    const int close_rc = close(unwritable);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (sink >= 0) {
    const int close_rc = close(sink);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }
  if (stderr_capture != NULL) {
    has_plumbing_failed =
        read_capture(stderr_capture, stderr_out, sizeof(stderr_out)) != 0 || has_plumbing_failed;
    const int close_rc = fclose(stderr_capture);
    TEST_CHECK(close_rc == 0);
    has_plumbing_failed = has_plumbing_failed || close_rc != 0;
  }

  TEST_CHECK(!has_plumbing_failed);
  TEST_CHECK(rc == EXIT_CODE_FAILURE);
  // `EBADF`: the writes go to a descriptor opened read-only. Deriving the reason from libc keeps
  // the assertion portable while the complete line comparison pins the command's own prefix.
  char reason[ERROR_MESSAGE_SIZE];
  char expected[ERROR_MESSAGE_SIZE * 2];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to write rewritten file to 'stdout': %s\n",
               error_system_message(reason, sizeof(reason), EBADF));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(strcmp(stderr_out, expected) == 0);

  (void)unlink(file_path);
  free(file_path);
}

TEST_LIST = {
    {"prints rewritten source", test_prints_rewritten_source},
    {"reports check change", test_reports_check_change},
    {"rewrites in place", test_rewrites_in_place},
    {"reports missing file", test_reports_missing_file},
    {"reports unwritable stdout", test_reports_unwritable_stdout},
    {NULL, NULL},
};
