#ifndef CWRAP_TEST_SUPPORT_H
#define CWRAP_TEST_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Result a test wrapper returns when its own setup or teardown fails rather than the call it wraps.
 * It is none of the `0`, `1` and `2` exit codes and neither the `0` nor the `-1` an action returns,
 * so a plumbing failure cannot pass for a result of the call under test.
 */
enum { TEST_PLUMBING_FAILED = 255 };

/**
 * Exit status of the unit-test build's `os_oom` when the test expects it, which `test_support.c`
 * defines in place of the one in `app/main.c`. It is none of the exit codes `cwrap` returns and no
 * signal status, so `test_is_oom` can tell running out of memory apart from a crash or an ordinary
 * exit. A process that runs out without `test_is_oom` expecting it exits with status `1` instead,
 * the only status acutest counts as a failed test, so the test fails rather than passing silently.
 */
enum { TEST_OOM_EXIT_STATUS = 86 };

/** Bytes of address space each `test_arena` reserves. Untouched pages cost no memory. */
enum { TEST_ARENA_LEN = 64 * 1024 * 1024 };

/**
 * @brief Returns an arena over a fresh reservation of `TEST_ARENA_LEN` bytes.
 *
 * Each call reserves a new block, so two arenas from two calls never overlap, as a long-lived and a
 * scratch arena must not. `test_arena` never releases the block. A block nobody passes to
 * `arena_release` goes back when acutest's child process for the test exits.
 *
 * @return An arena over the new block.
 */
struct Arena test_arena(void);

/**
 * @brief Returns an empty log over a fresh reservation of its own.
 *
 * The log's arena lives in the reservation it covers, so the log needs no storage from the caller
 * and shares no bytes with any arena the code under test is given.
 *
 * @return An empty log.
 */
struct Log test_log(void);

/**
 * @brief Reports whether a log holds exactly the text that a `printf` format produces, printing
 *        what it holds when it does not.
 *
 * @param log    Log to compare. Must not be `NULL`.
 * @param format `printf` format of the text the log must hold, every message ending in `\n`. Must
 *               not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `true` when the log's text is the formatted text.
 */
bool test_log_is(const struct Log* log, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(1, 2)));

/**
 * @brief Reports whether `text` is exactly the text that a `printf` format produces, printing what
 *        it is when it is not.
 *
 * @param text   Text to compare, such as what a host printed.
 * @param format `printf` format of the text `text` must be. Must not be `NULL`.
 * @param ...    Arguments that the format converts.
 * @return `true` when `text` is the formatted text.
 */
bool test_text_is(struct Str text, const char* format, ...)
    __attribute__((format(printf, 2, 3), nonnull(2)));

/**
 * @brief Reports whether a log holds exactly one error, `message`, printing what it holds when it
 *        does not.
 *
 * @param log     Log to compare. Must not be `NULL`.
 * @param message Terminated message the log must hold alone, without its newline. Must not be
 *                `NULL`.
 * @return `true` when the log holds `message` and nothing else, and counts one error.
 */
bool test_log_is_one(const struct Log* log, const char* message) __attribute__((nonnull(1, 2)));

/**
 * @brief Reports whether a log holds no text, printing what it holds when it does not.
 *
 * An empty format would make GCC's `-Wformat-zero-length` refuse `test_log_is(log, "")`.
 *
 * @param log Log to inspect. Must not be `NULL`.
 * @return `true` when the log holds no text.
 */
bool test_log_is_empty(const struct Log* log) __attribute__((nonnull(1)));

/**
 * @brief Reports whether running `fn` ends in `os_oom`.
 *
 * Runs `fn` in a child process, because `os_oom` ends the process it runs in. Only in that child
 * does `os_oom` exit with `TEST_OOM_EXIT_STATUS`. A child that returns from `fn`, crashes, or exits
 * any other way does not count.
 *
 * @param fn       Function to run in the child. Must not be `NULL`.
 * @param userdata Opaque argument forwarded to `fn`.
 * @return `true` when the child exited with `TEST_OOM_EXIT_STATUS`, or `false` otherwise.
 */
bool test_is_oom(void (*fn)(void* userdata), void* userdata);

/** Seed every seeded test starts its generator from. */
#define TEST_RNG_SEED_DEFAULT UINT64_C(20261005)

/**
 * A small deterministic random generator for seeded tests that run in the normal suite: a 64-bit
 * linear congruential step whose multiplier is the first hexadecimal digits of pi. It is no
 * statistical tool, only a cheap way to reach inputs nobody wrote down, the same ones on every run.
 */
struct TestRng {
  /** Current state. Every value is valid. */
  uint64_t state;
};

/**
 * @brief Advances the generator and returns 32 random bits.
 *
 * @param rng Generator to advance. Must not be `NULL`.
 * @return The high half of the new state, which an LCG mixes best.
 */
uint32_t test_rng_next(struct TestRng* rng) __attribute__((nonnull(1)));

/**
 * @brief Returns a value in `[0, bound)`.
 *
 * @param rng   Generator to advance. Must not be `NULL`.
 * @param bound Exclusive upper bound. Must be positive and at most `UINT32_MAX`.
 * @return The value.
 */
ptrdiff_t test_rng_below(struct TestRng* rng, ptrdiff_t bound) __attribute__((nonnull(1)));

/**
 * @brief Returns a copy of `text` that a failing check's message can show whole.
 *
 * `%.*s` stops at a `NUL` and passes other bytes through raw, so every byte outside printable
 * ASCII, and the backslash, is written as `\xNN` instead, with two lowercase hexadecimal digits.
 *
 * @param text  Text to show, which may hold any byte.
 * @param arena Arena that owns the copy. Must not be `NULL`.
 * @return The copy.
 */
struct Str test_printable(struct Str text, struct Arena* arena) __attribute__((nonnull(2)));

/**
 * @brief Creates a temporary fixture root.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path. Must not be
 *                 `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
int init_fixture_dir(char root_dir[static 1]);

/**
 * @brief Removes a fixture root and everything below it.
 *
 * Walking the tree keeps cleanup complete without a per-test inventory of the paths a fixture
 * writes or the code under test creates, and it needs no caller to classify a path as a file or a
 * directory. Each removal is checked, so a fixture a test leaves behind fails that test rather than
 * accumulating under `/tmp`.
 *
 * @param root_dir Fixture root directory to remove. Must not be `NULL`.
 */
void remove_fixture_tree(const char* root_dir);

/**
 * @brief Creates a temporary fixture file holding `contents`.
 *
 * The caller unlinks the file.
 *
 * @param file_path Writable `mkstemp` template. Receives the created file path. Must not be `NULL`.
 * @param contents  Bytes to write.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with no file left
 *         behind.
 */
int init_fixture_file(char file_path[static 1], struct Str contents);

/** A standard stream redirected by `capture_begin` or `capture_begin_unwritable`. */
struct StreamCapture {
  /** Redirected stream, `stdout` or `stderr`. */
  FILE* stream;

  /** Duplicate of the stream's original descriptor, which `capture_end` restores and closes. */
  int saved_fd;

  /** Anonymous file that receives the stream, or `NULL` when the stream is unwritable. */
  FILE* capture;
};

/**
 * @brief Redirects a standard stream into an anonymous file until `capture_end`.
 *
 * Flushes the stream first, so only text written after the call is captured.
 *
 * @param stream      Stream to redirect, `stdout` or `stderr`. Must not be `NULL`.
 * @param capture_out Receives the redirection state on success. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with the stream left
 *         unredirected.
 */
int capture_begin(FILE* stream, struct StreamCapture* capture_out);

/**
 * @brief Redirects a standard stream to a read-only descriptor until `capture_end`.
 *
 * Every write to the stream's descriptor fails with `EBADF`, whether `os_print` makes it directly
 * or stdio makes it when it flushes, which makes an output failure deterministic. `capture_end`
 * discards the text failed stdio writes leave buffered, so it never reaches the restored stream.
 *
 * @param stream      Stream to redirect, `stdout` or `stderr`. Must not be `NULL`.
 * @param capture_out Receives the redirection state on success. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with the stream left
 *         unredirected.
 */
int capture_begin_unwritable(FILE* stream, struct StreamCapture* capture_out);

/**
 * @brief Restores a redirected stream and reads what it captured.
 *
 * Flushes what stdio holds for the stream into the capture, restores the original descriptor, and
 * clears the stream's error indicator, which a stdio write to an unwritable stream sets. A write
 * through `os_print` reaches the descriptor at once and leaves nothing buffered.
 *
 * @param capture      State `capture_begin` or `capture_begin_unwritable` wrote. Must not be
 *                     `NULL`.
 * @param text_out     Buffer that receives the terminated captured text. Unused, and may be `NULL`,
 *                     for a capture `capture_begin_unwritable` began.
 * @param text_out_len Size of `text_out` in bytes. Must be positive when `text_out` is used.
 * @return `0` on success, or `-1` after recording a test-plumbing failure. The stream is restored
 *         either way.
 */
int capture_end(struct StreamCapture* capture, char* text_out, ptrdiff_t text_out_len);

#endif
