#include "app/cmd_wrap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/wrap_input.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** Bytes in a mebibyte, the unit of the sizes below. */
#define CMD_WRAP_MIB ((ptrdiff_t)1024 * 1024)

// The run has a log, an input and a scratch arena of the sizes below. A size costs address space
// only, because pages are committed as an arena fills, and an input that outgrows an arena ends in
// `os_oom`.
_Static_assert(PTRDIFF_MAX > 0xffffffff, "the arenas below need a 64-bit address space");

/**
 * Bytes of the run's log, which holds one diagnostic per input, each quoting a path from the
 * command line. `ARG_MAX` bounds the inputs: Linux takes at most 6 MiB of arguments, counting each
 * argument's pointer, so about 600,000 inputs, whose diagnostics take under 40 MB even when every
 * path is one byte. Its count of lines grows with the inputs whatever their size, so it keeps this
 * size under an address-space limit.
 */
#define CMD_WRAP_LOG_LEN (64 * CMD_WRAP_MIB)

/**
 * Bytes of the input arena, which holds one source of at most `SOURCE_LEN_MAX` and its rewritten
 * output. The output grows linearly with the source for every shape but two, which grow with the
 * square of the source: a comment indented by about half its line, where a 140 KB file gives
 * 2.45 GB at `-w 20`, and a Doxygen `@param` with a long name, where a 60,033-byte file gives
 * 400 MB at `-w 1`. A buffer at most doubles past what it holds, so 8 GiB holds an output of up to
 * 4 GiB.
 */
#define CMD_WRAP_INPUT_LEN (8 * 1024 * CMD_WRAP_MIB)

/**
 * Bytes of the scratch, which holds one rewrite's temporaries: under 90 bytes for each source byte
 * in every linear shape measured, so under 6 GiB at the limit. The long `@param` name shape grows
 * with the square of the source here too, about 6,685 bytes for each byte of that 60,033-byte file,
 * so a larger such file can outgrow the scratch and end in `os_oom`.
 */
#define CMD_WRAP_SCRATCH_LEN (8 * 1024 * CMD_WRAP_MIB)

/**
 * @brief Reads every path and drops the bytes, logging each one that cannot be read, in path order.
 *
 * It is the pass without writes before in-place mode writes anything, so a bad input among many
 * leaves every file untouched. It needs no rewrap, because the rewrap cannot fail.
 *
 * @param options Wrap knobs, in file mode. Must not be `NULL`.
 * @param os      Host the paths are read from. Must not be `NULL`.
 * @param input   Arena each read runs in, reused by the next.
 * @param log     Log that receives each read failure, in path order. Must not be `NULL`.
 */
static void cmd_wrap_run_read_inputs(const struct WrapOptions* options,
                                     struct Os* os,
                                     struct Arena input,
                                     struct Log* log) __attribute__((nonnull(1, 2, 4)));

/**
 * @brief Runs every selected input in the two arenas, logging their failures to `log`.
 *
 * @param options Wrap knobs. Must not be `NULL`.
 * @param os      Host the inputs are read from and written to. Must not be `NULL`.
 * @param input   Arena for one source and its rewritten output, reused by each input.
 * @param scratch Arena for the temporaries of one input, reused by each input.
 * @param log     Log that receives every failure and, in check mode, every change. Must not be
 *                `NULL`.
 * @return `0` when every input passed, or `-1` when one failed or, in check mode, would change.
 */
static int cmd_wrap_run_inputs(const struct WrapOptions* options,
                               struct Os* os,
                               struct Arena input,
                               struct Arena scratch,
                               struct Log* log) __attribute__((nonnull(1, 2, 5)));

enum ExitCode cmd_wrap_run(const struct WrapOptions* options, struct Os* os) {
  struct Arena log_block;
  if (arena_reserve(CMD_WRAP_LOG_LEN, &log_block) != 0) {
    os_oom();
  }
  struct Arena block;
  const int shift = arena_reserve_halving(CMD_WRAP_INPUT_LEN + CMD_WRAP_SCRATCH_LEN, &block);
  struct Arena scratch = block;
  const struct Arena input = arena_split(&scratch, CMD_WRAP_INPUT_LEN >> shift);
  struct Arena log_arena = log_block;
  struct Log log = {.buf = {.arena = &log_arena}};
  memory_oom_set_log(&log);
  if (!options->is_stdin && options->is_in_place) {
    cmd_wrap_run_read_inputs(options, os, input, &log);
  }
  const int rc = log.errors > 0 ? -1 : cmd_wrap_run_inputs(options, os, input, scratch, &log);
  if (rc != 0) {
    // The log is the whole report, each error on its own line. A report that cannot be printed has
    // nowhere else to go, and the exit code still says the run failed.
    (void)os_print(os, 2, buf_str(&log.buf));
  }
  // The log is in memory released below, and running out of memory after the inputs must not blame
  // the last input's phase.
  memory_oom_set_log(NULL);
  memory_oom_set_job((struct Str){0}, (struct Str){0}, false);
  arena_release(block);
  arena_release(log_block);
  return rc == 0 ? EXIT_CODE_OK : EXIT_CODE_FAILURE;
}

static void cmd_wrap_run_read_inputs(const struct WrapOptions* options,
                                     struct Os* os,
                                     struct Arena input,
                                     struct Log* log) {
  for (ptrdiff_t i = 0; i < options->path_count; i++) {
    // Each read gets a copy of the input arena, so the next one reuses its memory.
    struct Arena arena = input;
    struct Str source;
    (void)wrap_input_read_file(str_from_cstr(options->paths[i]), os, &arena, &source, log);
  }
}

static int cmd_wrap_run_inputs(const struct WrapOptions* options,
                               struct Os* os,
                               struct Arena input,
                               struct Arena scratch,
                               struct Log* log) {
  if (options->is_stdin) {
    const enum WrapInputResult result =
        wrap_input_process_stdin(options->width, options->is_check, os, input, scratch, log);
    return result == WRAP_INPUT_RESULT_OK ? 0 : -1;
  }
  bool has_failed = false;
  for (ptrdiff_t i = 0; i < options->path_count; i++) {
    const enum WrapInputResult result =
        wrap_input_process_file(str_from_cstr(options->paths[i]), options->width,
                                options->is_in_place, options->is_check, os, input, scratch, log);
    switch (result) {
      case WRAP_INPUT_RESULT_OK:
        continue;
      case WRAP_INPUT_RESULT_CHANGED:
      case WRAP_INPUT_RESULT_ERROR:
        has_failed = true;
        break;
    }
    if (!options->is_check) {
      break;
    }
  }
  return has_failed ? -1 : 0;
}
