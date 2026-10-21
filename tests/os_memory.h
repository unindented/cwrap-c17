#ifndef CWRAP_OS_MEMORY_H
#define CWRAP_OS_MEMORY_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/str.h"

struct Os;

// `os_memory.c` is the in-memory implementation of `src/runtime/os.h`. A unit test linking it sets
// up its files and standard input with the functions below and passes the host to the code under
// test, which then reads and writes those files and prints as it would on the real host. Nothing
// touches the filesystem or the process's descriptors, so a test needs no temporary file, no
// cleanup, and no redirection of a standard stream.
//
// `cwrap` takes paths and never lists a directory, so the host is a set of files keyed by the exact
// path text the code passes, with no directory tree, no symlinks and no resolution of `.` or `..`:
// two spellings of one file are two files here. Without a tree, a write needs no parent directory,
// where the POSIX host fails one whose parent is missing (`ENOENT`) or is a file (`ENOTDIR`), so
// those cases are tested in `src/runtime/test_fs.c` only. A path can also name a directory, which
// holds nothing and is there to be refused as not a regular file. Failures the files cannot produce
// by their shape, such as `EACCES`, `EIO` or `ENOSPC`, are injected with `os_memory_fail`.
//
// Each operation the code under test calls produces the message the POSIX implementation would for
// the same situation, so a test written against this host asserts the text a user sees.
// `src/runtime/test_os.c` runs the same tests against both hosts to keep them in agreement: a
// property of the files and streams that a test relies on here needs a test there. An injected
// failure is not one of those properties. It is reported with the message the POSIX implementation
// gives for that `errno` value.

/** Which operation an injected failure applies to. */
enum OsMemoryOp {
  /** Opening the path in `os_read`, as for a file the process may not read. */
  OS_MEMORY_OP_OPEN,

  /** Reading the bytes of an opened path in `os_read`, as for a device that fails mid-read. */
  OS_MEMORY_OP_READ,

  /** `os_write` of the path, which leaves the file there as it was, as an atomic write does. */
  OS_MEMORY_OP_WRITE,
};

/**
 * @brief Returns a new host with no files, an empty standard input, and nothing printed.
 *
 * The host lives in a reservation of its own, which acutest's child process returns when the test
 * ends.
 *
 * @return The host, never `NULL`.
 */
struct Os* os_memory(void) __attribute__((returns_nonnull));

/**
 * @brief Writes a regular file as a fixture, replacing anything at the path.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Terminated path of the file. Must not be `NULL`.
 * @param contents  Terminated file contents. Must not be `NULL`.
 */
void os_memory_write(struct Os* os, const char* file_path, const char* contents)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Writes a regular file whose contents may hold any byte, replacing anything at the path.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Path of the file.
 * @param contents  File contents.
 */
void os_memory_write_bytes(struct Os* os, struct Str file_path, struct Str contents)
    __attribute__((nonnull(1)));

/**
 * @brief Extends a regular file to `len` bytes, as `truncate` does, as a fixture.
 *
 * The bytes past the old end read as `NUL`, so a read that passes the size checks rejects them as
 * an embedded `NUL`. The host stores only their count, so a test can model a source far larger than
 * its memory, as a sparse file does on disk.
 *
 * @param os        Host holding the file. Must not be `NULL`.
 * @param file_path Terminated path of an existing regular file. Must not be `NULL`.
 * @param len       New size in bytes. Must not be below the file's size.
 */
void os_memory_truncate(struct Os* os, const char* file_path, ptrdiff_t len)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Creates a directory at `dir_path`, as a fixture that is not a regular file.
 *
 * The directory holds nothing, because the host has no tree: a path below it is a path of its own.
 *
 * @param os       Host to create the directory on. Must not be `NULL`.
 * @param dir_path Terminated path of the directory. Must not be `NULL`.
 */
void os_memory_mkdir(struct Os* os, const char* dir_path) __attribute__((nonnull(1, 2)));

/**
 * @brief Gives standard input its bytes, and how a read past them ends.
 *
 * @param os           Host whose standard input to set. Must not be `NULL`.
 * @param contents     Bytes standard input holds.
 * @param error_number `0` for an end of input after `contents`, or the `errno` value a read past
 *                     them fails with.
 */
void os_memory_stdin(struct Os* os, struct Str contents, int error_number)
    __attribute__((nonnull(1)));

/**
 * @brief Makes every later `op` on exactly `path` fail with `error_number`.
 *
 * The match is on the path text the code under test passes, as every lookup of this host is.
 *
 * @param os           Host to inject into. Must not be `NULL`.
 * @param op           Operation that fails.
 * @param path         Terminated path the failure applies to. Must not be `NULL`.
 * @param error_number `errno` value the operation fails with.
 */
void os_memory_fail(struct Os* os, enum OsMemoryOp op, const char* path, int error_number)
    __attribute__((nonnull(1, 3)));

/**
 * @brief Makes `os_print` on `fd` fail with `error_number` once `len` bytes have been printed
 *        there.
 *
 * The call that crosses `len` prints the bytes up to it and then fails, as a short write followed
 * by a failing one does on a descriptor, and every later call on `fd` fails without printing.
 *
 * @param os           Host to inject into. Must not be `NULL`.
 * @param fd           `1` for standard output or `2` for standard error.
 * @param len          Bytes the stream accepts before it fails. Must not be negative.
 * @param error_number `errno` value the print fails with, such as `EPIPE` or `EIO`.
 */
void os_memory_fail_print(struct Os* os, int fd, ptrdiff_t len, int error_number)
    __attribute__((nonnull(1)));

/**
 * @brief Reports whether `file_path` names a regular file holding exactly `contents`.
 *
 * Records what the file holds, or that it is missing, when it does not match.
 *
 * @param os        Host to inspect. Must not be `NULL`.
 * @param file_path Terminated path of the file. Must not be `NULL`.
 * @param contents  Terminated contents the file must hold. Must not be `NULL`.
 * @return `true` when the file exists and holds `contents`.
 */
bool os_memory_is_file(struct Os* os, const char* file_path, const char* contents)
    __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Returns everything `os_print` has written to `fd` so far.
 *
 * @param os Host to inspect. Must not be `NULL`.
 * @param fd `1` for standard output or `2` for standard error.
 * @return The text, which the host owns.
 */
struct Str os_memory_printed(struct Os* os, int fd) __attribute__((nonnull(1)));

/**
 * @brief Reports whether `os_print` has written exactly `expected` to `fd` so far.
 *
 * Records what the stream holds when it does not match.
 *
 * @param os       Host to inspect. Must not be `NULL`.
 * @param fd       `1` for standard output or `2` for standard error.
 * @param expected Terminated text the stream must hold. Must not be `NULL`.
 * @return `true` when the stream holds exactly `expected`.
 */
bool os_memory_is_printed(struct Os* os, int fd, const char* expected)
    __attribute__((nonnull(1, 3)));

#endif
