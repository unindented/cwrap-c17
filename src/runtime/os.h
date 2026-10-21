#ifndef CWRAP_OS_H
#define CWRAP_OS_H

#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;

// Every operation the program performs on the host goes through the functions below, and nothing
// else in `src/` makes a system call on a path or a standard stream. Each is declared once here and
// defined by exactly one implementation per executable, chosen when it is linked, as `_ssg` does
// with `main_posix.cpp` and `main_test.cpp`:
//
// - `runtime/fs.c` is the POSIX implementation, which the `cwrap` executable and every unit test
//   registered without `OS memory` link (`cmake/cwrap_testing.cmake` says which tests those are and
//   why).
// - `tests/os_memory.c` is an in-memory implementation, which the unit tests registered with
//   `OS memory` link, so they set up their inputs and read back their output without touching the
//   disk or the process's descriptors.
//
// A link-time choice needs no table of function pointers, and the program never needs two
// implementations in one process. `struct Os` is defined by the implementation, so each carries its
// own state: none for POSIX, the files and streams for the in-memory one.
//
// `os_oom`, the remaining member of the interface, is declared in `shared/arena.h`, because the
// arena calls it. It is not defined here but by each program: `app/main.c` for the executable and
// `tests/test_support.c` for every unit test, whichever implementation it links.
//
// Every path is a `struct Str`. A path the POSIX system could not take whole fails rather than
// naming some other file: one of `PATH_MAX` bytes or more fails as `ENAMETOOLONG`, as the system
// would fail it, and one holding a `NUL` byte fails as `EINVAL`, because the system would read only
// the bytes before it.
//
// Every operation below that can fail and takes a `log` also takes the caller's `operation`, such
// as `failed to read file`, and records one error in `log` that leads with it and then gives the
// cause, as in `failed to read file: No such file or directory ('src/a.c')`. The caller owns the
// operation, because `wrap_input` reports a failed `os_read` as `failed to read file` and a failed
// `os_write` as `failed to write file`, while only the implementation knows the cause. First-party
// causes are lowercase. Operating-system messages keep their spelling.

/** The host the program runs against. Each implementation defines it. */
struct Os;

/**
 * @brief Reads a regular file into `perm`.
 *
 * It writes the output parameter only on success, and a failed read leaves `perm` as it found it.
 * It rejects a file that is not regular, one larger than `data_len_max`, and one that changes size
 * during the read. It never reports a partial copy as a successful read. A FIFO is rejected rather
 * than waited on. The size limit is checked before any byte is read or allocated, so an oversize
 * input never becomes resident. Each caller passes the limit for the kind of file it reads.
 *
 * It also rejects a file containing an embedded `NUL` byte. A `NUL` is an ordinary byte to every
 * first-party string, but no input format `cwrap` reads gives it a meaning, so an input carrying
 * one is rejected here rather than passed through to the output.
 *
 * @param os           Host to read from. Must not be `NULL`.
 * @param file_path    Path of the file to read.
 * @param data_len_max Largest accepted file size in bytes.
 * @param perm         Arena that owns the file bytes. Must not be `NULL`.
 * @param data_out     Receives the file bytes. Must not be `NULL`.
 * @param operation    Operation leading the failure message, such as `failed to read file`. The
 *                     message names `file_path`, and its cause tells the first-party rejections
 *                     apart from the system ones.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` when the file is missing, not regular, larger than
 *         `data_len_max`, changed size mid-read or contains an embedded `NUL`, and on an open, read
 *         or close failure.
 */
int os_read(struct Os* os,
            struct Str file_path,
            ptrdiff_t data_len_max,
            struct Arena* perm,
            struct Str* data_out,
            struct Str operation,
            struct Log* log) __attribute__((nonnull(1, 4, 5, 7)));

/**
 * @brief Reads standard input to its end into `perm`.
 *
 * Standard input need not be seekable and is not closed. The function writes the output parameter
 * only on success, and a failed read leaves `perm` as it found it. It rejects input longer than
 * `data_len_max`, and stops reading once it has seen one byte past that limit, so the limit also
 * bounds the memory the read takes. It rejects embedded `NUL` bytes, the same input policy
 * `os_read` applies to regular files.
 *
 * The bytes grow in place at the end of `perm`, which nothing else may allocate from during the
 * read, so they are never copied. The buffer may keep up to one read step of unused bytes past the
 * end.
 *
 * A failure is recorded in `log` as one message: the caller's `operation`, the cause, and the
 * stream's name, as in `failed to read input: contains an embedded NUL byte (from 'stdin')`.
 *
 * @param os           Host to read from. Must not be `NULL`.
 * @param data_len_max Largest accepted input length in bytes. Must not be negative.
 * @param perm         Arena that owns the bytes. Must not be `NULL`.
 * @param data_out     Receives the bytes. Must not be `NULL`.
 * @param operation    Operation leading the failure message, such as `failed to read input`.
 * @param log          Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a read failure, input longer than `data_len_max`, or an
 *         embedded `NUL`.
 */
int os_read_stdin(struct Os* os,
                  ptrdiff_t data_len_max,
                  struct Arena* perm,
                  struct Str* data_out,
                  struct Str operation,
                  struct Log* log) __attribute__((nonnull(1, 3, 4, 6)));

/**
 * @brief Atomically writes `data` to `file_path`.
 *
 * `cwrap` rewrites a user's source file in place, so a failed or interrupted write must leave the
 * previous file whole. The write goes to a sibling temporary file, which is written, synced and
 * closed before it is renamed over the destination, so a failed write leaves an existing file
 * untouched. It follows a symbolic link rather than replacing it. A dangling symbolic link fails,
 * with the `ENOENT` that `realpath` reports, rather than creating the file the link names. It does
 * not create missing parent directories.
 *
 * A reader sees either the previous complete file or the new complete file. The temporary is
 * `fsync`ed before the `rename`, so its bytes reach the device before the name that publishes them
 * does. Reversed, a power loss could leave the destination empty or truncated. Two limits remain.
 * This uses `fsync` and not `F_FULLFSYNC`, so a drive that acknowledges a flush while the data sits
 * in its own volatile cache can still lose it. The containing directory is not synced either, so a
 * crash can undo the `rename` and leave the previous complete file in place, which is the safe
 * direction to fail in: rerunning cwrap redoes the rewrite.
 *
 * An existing destination keeps its permission bits. A newly created file gets mode `0666` reduced
 * by the process umask, the mode `fopen(path, "wb")` would give it. Because `rename` installs a new
 * inode rather than writing through the old one, replacement breaks any hard link to the
 * destination, which keeps the previous contents, and does not preserve the destination's owner,
 * group, ACLs or extended attributes.
 *
 * This is the only operation that changes the host's files, and should stay the only one.
 *
 * @param os        Host to write to. Must not be `NULL`.
 * @param file_path Destination path.
 * @param data      Bytes to write.
 * @param operation Operation leading the failure message, which names `file_path`.
 * @param log       Log that receives the failure. Must not be `NULL`.
 * @return `0` on success, or `-1` on a metadata, temporary-file, write, sync, close, or rename
 *         failure.
 */
int os_write(struct Os* os,
             struct Str file_path,
             struct Str data,
             struct Str operation,
             struct Log* log) __attribute__((nonnull(1, 5)));

/**
 * @brief Writes `text` to standard output (`1`) or standard error (`2`).
 *
 * Every byte the program prints goes through here: the rewritten source in the default mode, the
 * log a run ends with, a rejected command line, and `--help` and `--version`, so a test on the
 * in-memory host can capture all of it. Nothing in `src/` prints through `stdio`, whose buffer
 * `_Exit` would drop.
 *
 * One call writes its text through one descriptor, retrying a short or interrupted write, and
 * nothing else in the program buffers it.
 *
 * @param os   Host to print on. Must not be `NULL`.
 * @param fd   `1` for standard output or `2` for standard error.
 * @param text Bytes to write.
 * @return `0` once every byte is written, or `-1` with `errno` set on a write failure.
 */
int os_print(struct Os* os, int fd, struct Str text) __attribute__((nonnull(1)));

#endif
