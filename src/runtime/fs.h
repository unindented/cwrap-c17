#ifndef CWRAP_FS_H
#define CWRAP_FS_H

#include <stddef.h>

/**
 * Size in bytes of a filesystem failure reason, including the `NUL` terminator.
 *
 * These actions report a reason fragment, not a whole diagnostic, because the caller owns the
 * operation and the attribution. `wrap_input` reports a failed `fs_read_file` as
 * `failed to read file` and a failed `fs_write_file` as `failed to write file`. First-party
 * fragments are lowercase and unquoted. Operating-system messages keep their original spelling.
 * Both compose as `<caller's operation>: <reason>`, with any unbounded path trailing the reason,
 * never leading it. `ERROR_MESSAGE_SIZE` is smaller than a path may be, so a leading path would
 * truncate the cause off the end.
 */
enum { FS_REASON_SIZE = 256 };

/**
 * @brief Reads a regular file into a freshly allocated, `NUL`-terminated buffer.
 *
 * On success the caller owns `*data_out` and must `free` it. It writes both output parameters only
 * on success. It rejects non-regular files and files that change size during a read. It never
 * reports a partial copy as a successful read.
 *
 * It opens the path without blocking and checks the opened descriptor, so the file inspected is the
 * file read and a FIFO is rejected rather than waited on.
 *
 * It also rejects a file containing an embedded `NUL` byte. This is the boundary that establishes
 * the codebase's text invariant. Every owned string is a `NUL`-free C string, so downstream payload
 * helpers can use terminated-string operations without truncating the source.
 *
 * @param file_path    Path of the file to read. Must not be `NULL`.
 * @param data_out     Receives the malloc'd buffer holding the file bytes plus a terminator. Must
 *                     not be `NULL`.
 * @param data_len_out Receives the number of bytes read, excluding the terminator. Must not be
 *                     `NULL`.
 * @param reason       Receives the failure reason, which tells the first-party rejections apart
 *                     from the system ones. May be `NULL` only when `reason_len` is 0. Untouched on
 *                     success.
 * @param reason_len   Size of `reason` in bytes.
 * @return `0` on success, or `-1` when the file is missing, not regular, too large, changed size
 *         mid-read or contains an embedded `NUL`, and on an open, read, allocation or close
 *         failure.
 */
int fs_read_file(const char* file_path,
                 char** data_out,
                 size_t* data_len_out,
                 char* reason,
                 size_t reason_len) __attribute__((nonnull(1, 2, 3)));

/**
 * @brief Atomically writes `data_len` bytes to `file_path`.
 *
 * Writes and closes a sibling temporary file before renaming it over the destination, so a failed
 * write leaves an existing file untouched. Follows a symbolic link rather than replacing it. Does
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
 * @param file_path  Destination path. Must not be `NULL`.
 * @param data       Source bytes. Must hold at least `data_len` bytes. Must not be `NULL`.
 * @param data_len   Number of bytes to write.
 * @param reason     Receives the failure reason. May be `NULL` only when `reason_len` is 0.
 *                   Untouched on success.
 * @param reason_len Size of `reason` in bytes.
 * @return `0` on success, or `-1` on an allocation, metadata, temporary-file, write, sync, close,
 *         or rename failure.
 */
int fs_write_file(const char* file_path,
                  const char* data,
                  size_t data_len,
                  char* reason,
                  size_t reason_len) __attribute__((nonnull(1, 2)));

#endif
