#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/** Largest file a test here reads back, in bytes. Every fixture file is a few bytes. */
enum { TEST_FILE_LEN_MAX = 1024 };

/** Operation of every call whose failure is a broken fixture, which `fixture_log` receives. */
#define FIXTURE_OPERATION S("failed to set up fixture")

/**
 * @brief Reports whether a directory holds exactly the given number of entries.
 *
 * The `.` and `..` entries are not counted.
 *
 * @param dir_path    Directory to list. Must not be `NULL`.
 * @param entry_count Number of entries the directory must hold.
 * @return `true` when the directory holds exactly `entry_count` entries, or `false` when it holds
 *         another number or cannot be listed.
 */
static bool has_entry_count(const char* dir_path, ptrdiff_t entry_count) {
  DIR* dir = opendir(dir_path);
  if (dir == NULL) {
    return false;
  }
  ptrdiff_t found_count = 0;
  const struct dirent* entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      found_count++;
    }
  }
  return closedir(dir) == 0 && found_count == entry_count;
}

/**
 * @brief Returns the log of every call whose failure is a broken fixture rather than behavior under
 *        test, such as writing a fixture file or a read whose result alone is checked.
 *
 * One log serves every such call in a test process. Each failing call is also a failed check, so
 * the log adds nothing a test asserts on.
 *
 * @return The shared log.
 */
static struct Log* fixture_log(void) {
  static struct Arena arena;
  static struct Log log;
  if (log.buf.arena == NULL) {
    arena = test_arena();
    log.buf.arena = &arena;
  }
  return &log;
}

// These tests check what only a real filesystem has: FIFOs, modes, permission bits, symbolic links,
// parent directories and path resolution. The contract tests in `test_os.c` check the rest of
// `os.h` against this host and the in-memory one.

// A path one byte short of `PATH_MAX` still has room for its terminator, so it reaches the system
// call and reads the file it names. `/.` components pad the fixture's resolved directory out to
// that length. It is resolved first so that no symlink in it, such as a `/tmp` that is one,
// lengthens it on the way.
static void test_read_accepts_path_below_path_max(void) {
  char file_path[] = "/tmp/cwrap-fs-path-limit.XXXXXX";
  if (init_fixture_file(file_path, S("a")) != 0) {
    return;
  }
  char resolved_path[PATH_MAX];
  const char* basename = NULL;
  struct Str name = {0};
  ptrdiff_t len = 0;
  static char long_path[PATH_MAX - 1];
  struct Arena arena = test_arena();
  struct Str file_data = {0};
  if (!TEST_CHECK(realpath(file_path, resolved_path) != NULL)) {
    goto cleanup;
  }
  basename = strrchr(resolved_path, '/');
  if (!TEST_CHECK(basename != NULL)) {
    goto cleanup;
  }
  name = str_from_cstr(basename);
  len = basename - resolved_path;
  memcpy(long_path, resolved_path, (size_t)len);
  // A doubled `/` evens out the padding, so the `/.` pairs end exactly where the name starts.
  if ((PATH_MAX - 1 - len - name.len) % 2 != 0) {
    long_path[len++] = '/';
  }
  while (len < PATH_MAX - 1 - name.len) {
    long_path[len++] = '/';
    long_path[len++] = '.';
  }
  memcpy(long_path + len, name.data, (size_t)name.len);

  TEST_CHECK(os_read(os_posix(), (struct Str){long_path, PATH_MAX - 1}, TEST_FILE_LEN_MAX, &arena,
                     &file_data, FIXTURE_OPERATION, fixture_log()) == 0);
  TEST_CHECK(str_eq(file_data, S("a")));

cleanup:
  (void)unlink(file_path);
}

// A FIFO is rejected as not a regular file instead of blocking the open until a writer opens it,
// leaving the outputs untouched.
static void test_read_rejects_fifo(void) {
  char fifo_path[] = "/tmp/cwrap-fs-read-fifo.XXXXXX";
  if (init_fixture_file(fifo_path, S("")) != 0) {
    return;
  }
  (void)unlink(fifo_path);

  struct Arena arena = test_arena();
  const struct Str sentinel = S("unchanged");
  struct Str file_data = sentinel;
  struct Log log = test_log();
  if (!TEST_CHECK(mkfifo(fifo_path, 0600) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_read(os_posix(), str_from_cstr(fifo_path), TEST_FILE_LEN_MAX, &arena, &file_data,
                     S("failed to read file"), &log) == -1);
  TEST_CHECK(file_data.data == sentinel.data && file_data.len == sentinel.len);
  TEST_CHECK(test_log_is(&log, "failed to read file: not a regular file ('%s')\n", fifo_path));

cleanup:
  (void)unlink(fifo_path);
}

// A file the process may not read fails the read with `EACCES`.
static void test_read_rejects_unreadable_file(void) {
  // Root bypasses the permission bits, so the file would open like any other and the assertions
  // below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }
  char file_path[] = "/tmp/cwrap-fs-unreadable.XXXXXX";
  if (init_fixture_file(file_path, S("a")) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct Str file_data = {0};
  struct Log log = test_log();
  int read_rc = 0;
  if (TEST_CHECK(chmod(file_path, 0) == 0)) {
    read_rc = os_read(os_posix(), str_from_cstr(file_path), TEST_FILE_LEN_MAX, &arena, &file_data,
                      S("failed to read file"), &log);
  }
  // The file is removed before any assertion, so a failed one cannot leave it behind.
  (void)unlink(file_path);

  TEST_CHECK(read_rc == -1);
  TEST_CHECK(test_log_is(&log, "failed to read file: %s ('%s')\n", strerror(EACCES), file_path));
}

// `os_write` creates a new file with `0666` reduced by the process umask rather than `mkstemp`'s
// `0600`, and replacing an existing file keeps that file's mode. No temporary survives either
// write, which the count of entries left in the fixture directory checks.
static void test_write_applies_umask_and_keeps_existing_mode(void) {
  char root_dir[] = "/tmp/cwrap-fs-mode.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  char created[PATH_MAX];
  const int created_n = snprintf(created, sizeof(created), "%s/created.c", root_dir);
  char existing[PATH_MAX];
  const int existing_n = snprintf(existing, sizeof(existing), "%s/existing.c", root_dir);
  mode_t umask_before = 0;
  struct stat st;
  if (!TEST_CHECK(created_n > 0 && (size_t)created_n < sizeof(created))) {
    goto cleanup;
  }
  if (!TEST_CHECK(existing_n > 0 && (size_t)existing_n < sizeof(existing))) {
    goto cleanup;
  }

  // A umask other than the usual `022` is the case a hardcoded mode would discard. It also must not
  // be `077`, whose result is `mkstemp`'s own `0600` and so could not show the mode was applied.
  // Restore it before asserting, so a failure cannot leak the changed value into later tests.
  umask_before = umask(027);
  TEST_CHECK(
      os_write(os_posix(), str_from_cstr(created), S("x"), FIXTURE_OPERATION, fixture_log()) == 0);
  TEST_CHECK(
      os_write(os_posix(), str_from_cstr(existing), S("x"), FIXTURE_OPERATION, fixture_log()) == 0);
  TEST_CHECK(chmod(existing, 0640) == 0);
  TEST_CHECK(
      os_write(os_posix(), str_from_cstr(existing), S("y"), FIXTURE_OPERATION, fixture_log()) == 0);
  (void)umask(umask_before);

  TEST_CHECK(stat(created, &st) == 0 && (st.st_mode & 07777) == (mode_t)(0666 & ~027));
  TEST_CHECK(stat(existing, &st) == 0 && (st.st_mode & 07777) == 0640);
  TEST_CHECK(has_entry_count(root_dir, 2));

cleanup:
  remove_fixture_tree(root_dir);
}

// Atomic replacement follows a symbolic link and leaves the link itself in place.
static void test_write_follows_symbolic_link(void) {
  char target_path[] = "/tmp/cwrap-fs-link-target.XXXXXX";
  if (init_fixture_file(target_path, S("before")) != 0) {
    return;
  }
  char link_path[] = "/tmp/cwrap-fs-link.XXXXXX";
  if (init_fixture_file(link_path, S("")) != 0) {
    (void)unlink(target_path);
    return;
  }
  (void)unlink(link_path);
  struct Arena arena = test_arena();
  struct stat link_st;
  struct Str file_data = {0};
  if (!TEST_CHECK(symlink(target_path, link_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_write(os_posix(), str_from_cstr(link_path), S("after"), FIXTURE_OPERATION,
                      fixture_log()) == 0);
  if (!TEST_CHECK(lstat(link_path, &link_st) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(S_ISLNK(link_st.st_mode));
  TEST_CHECK(os_read(os_posix(), str_from_cstr(target_path), TEST_FILE_LEN_MAX, &arena, &file_data,
                     FIXTURE_OPERATION, fixture_log()) == 0);
  TEST_CHECK(str_eq(file_data, S("after")));

cleanup:
  (void)unlink(link_path);
  (void)unlink(target_path);
}

// `os_write` fails when the parent directory is missing, when a parent component is a regular file,
// and when the target is itself a directory, and reports the three as different system reasons.
// cwrap does not create parent directories, so an absent parent is a failure rather than something
// to fill in. A file in the parent position fails the destination `stat` with `ENOTDIR` before any
// temporary file is created.
static void test_write_rejects_missing_or_file_parent_and_dir_target(void) {
  char root_dir[] = "/tmp/cwrap-fs-write-fail.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  char missing_parent_path[PATH_MAX];
  const int missing_n =
      snprintf(missing_parent_path, sizeof(missing_parent_path), "%s/missing/x.c", root_dir);
  char blocking_file[PATH_MAX];
  const int blocking_n = snprintf(blocking_file, sizeof(blocking_file), "%s/plain.c", root_dir);
  char through_file[PATH_MAX];
  const int through_n = snprintf(through_file, sizeof(through_file), "%s/x.c", blocking_file);
  char dir_target[PATH_MAX];
  const int target_n = snprintf(dir_target, sizeof(dir_target), "%s/target", root_dir);
  struct Log missing_log = test_log();
  struct Log through_log = test_log();
  struct Log dir_log = test_log();
  if (!TEST_CHECK(missing_n > 0 && (size_t)missing_n < sizeof(missing_parent_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(blocking_n > 0 && (size_t)blocking_n < sizeof(blocking_file))) {
    goto cleanup;
  }
  if (!TEST_CHECK(through_n > 0 && (size_t)through_n < sizeof(through_file))) {
    goto cleanup;
  }
  if (!TEST_CHECK(target_n > 0 && (size_t)target_n < sizeof(dir_target))) {
    goto cleanup;
  }

  TEST_CHECK(os_write(os_posix(), str_from_cstr(missing_parent_path), S("x"),
                      S("failed to write file"), &missing_log) == -1);
  TEST_CHECK(test_log_is(&missing_log, "failed to write file: %s ('%s')\n", strerror(ENOENT),
                         missing_parent_path));

  TEST_CHECK(os_write(os_posix(), str_from_cstr(blocking_file), S("x"), FIXTURE_OPERATION,
                      fixture_log()) == 0);
  TEST_CHECK(os_write(os_posix(), str_from_cstr(through_file), S("x"), S("failed to write file"),
                      &through_log) == -1);
  TEST_CHECK(test_log_is(&through_log, "failed to write file: %s ('%s')\n", strerror(ENOTDIR),
                         through_file));

  if (!TEST_CHECK(mkdir(dir_target, 0700) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(os_write(os_posix(), str_from_cstr(dir_target), S("x"), S("failed to write file"),
                      &dir_log) == -1);
  TEST_CHECK(
      test_log_is(&dir_log, "failed to write file: %s ('%s')\n", strerror(EISDIR), dir_target));

cleanup:
  remove_fixture_tree(root_dir);
}

// A dangling symbolic link fails with the `ENOENT` that `realpath` reports instead of creating the
// file the link names, and nothing is left in the link's directory. Following the link to a missing
// target would otherwise write a file at a path the caller never passed.
static void test_write_rejects_dangling_symbolic_link(void) {
  char root_dir[] = "/tmp/cwrap-fs-dangling.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  char target_path[PATH_MAX];
  const int target_n = snprintf(target_path, sizeof(target_path), "%s/missing.c", root_dir);
  char link_path[PATH_MAX];
  const int link_n = snprintf(link_path, sizeof(link_path), "%s/link.c", root_dir);
  struct Log log = test_log();
  struct stat target_st;
  if (!TEST_CHECK(target_n > 0 && (size_t)target_n < sizeof(target_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(link_n > 0 && (size_t)link_n < sizeof(link_path))) {
    goto cleanup;
  }
  if (!TEST_CHECK(symlink(target_path, link_path) == 0)) {
    goto cleanup;
  }

  TEST_CHECK(os_write(os_posix(), str_from_cstr(link_path), S("x"), S("failed to write file"),
                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('%s')\n", strerror(ENOENT), link_path));
  TEST_CHECK(lstat(target_path, &target_st) != 0);
  // The link is the only entry, which proves no temporary was left behind.
  TEST_CHECK(has_entry_count(root_dir, 1));

cleanup:
  remove_fixture_tree(root_dir);
}

// A destination whose temporary sibling would not fit in `PATH_MAX` bytes fails as `ENAMETOOLONG`,
// as the system would fail the temporary itself, even though the destination path fits.
static void test_write_rejects_temporary_path_past_path_max(void) {
  static char long_path[PATH_MAX - 4];
  long_path[0] = '/';
  for (ptrdiff_t i = 1; i < (ptrdiff_t)(sizeof(long_path)); i++) {
    long_path[i] = i % 64 == 0 ? '/' : 'a';
  }
  long_path[sizeof(long_path) - 2] = '/';

  const struct Str path = {long_path, sizeof(long_path)};
  struct Log log = test_log();
  TEST_CHECK(os_write(os_posix(), path, S("x"), S("failed to write file"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write file: %s ('%.*s')\n", strerror(ENAMETOOLONG),
                         STR_FMT(path)));
}

TEST_LIST = {
    {"read accepts path below path max", test_read_accepts_path_below_path_max},
    {"read rejects fifo", test_read_rejects_fifo},
    {"read rejects unreadable file", test_read_rejects_unreadable_file},
    {"write applies umask and keeps existing mode",
     test_write_applies_umask_and_keeps_existing_mode},
    {"write follows symbolic link", test_write_follows_symbolic_link},
    {"write rejects missing or file parent and dir target",
     test_write_rejects_missing_or_file_parent_and_dir_target},
    {"write rejects dangling symbolic link", test_write_rejects_dangling_symbolic_link},
    {"write rejects temporary path past path max", test_write_rejects_temporary_path_past_path_max},
    {NULL, NULL},
};
