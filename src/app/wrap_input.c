#include "app/wrap_input.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "domain/rewrite.h"
#include "runtime/memory.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * @brief Rewrites and emits source bytes that have already been read.
 *
 * @param source      Original source bytes.
 * @param width       Wrapping column.
 * @param is_stream   Whether the source came from standard input rather than a file.
 * @param input_name  Name used in diagnostics.
 * @param output_path In-place destination, or empty to emit to standard output.
 * @param is_check    Whether to report a change without emitting it.
 * @param os          Host the output is written to or printed on. Must not be `NULL`.
 * @param input       Arena the source came from, which receives the rewritten output.
 * @param scratch     Arena for the rewrite's temporaries.
 * @param log         Log that receives every failure and a check-mode change. Must not be `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` for a check-mode change,
 *         or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
static enum WrapInputResult process_source(struct Str source,
                                           ptrdiff_t width,
                                           bool is_stream,
                                           struct Str input_name,
                                           struct Str output_path,
                                           bool is_check,
                                           struct Os* os,
                                           struct Arena input,
                                           struct Arena scratch,
                                           struct Log* log) __attribute__((nonnull(7, 10)));

/**
 * @brief Emits one rewritten source according to the selected output mode.
 *
 * @param source           Original source bytes.
 * @param rewritten_source Rewritten source to emit.
 * @param is_stream        Whether the source came from standard input rather than a file.
 * @param input_name       Name used in diagnostics.
 * @param output_path      In-place destination, or empty to emit to standard output.
 * @param is_check         Whether to report a change without emitting it.
 * @param os               Host the output is written to or printed on. Must not be `NULL`.
 * @param log              Log that receives every failure and a check-mode change. Must not be
 *                         `NULL`.
 * @return `WRAP_INPUT_RESULT_OK` on success, `WRAP_INPUT_RESULT_CHANGED` for a check-mode change,
 *         or `WRAP_INPUT_RESULT_ERROR` on failure.
 */
static enum WrapInputResult process_source_emit(struct Str source,
                                                struct Str rewritten_source,
                                                bool is_stream,
                                                struct Str input_name,
                                                struct Str output_path,
                                                bool is_check,
                                                struct Os* os,
                                                struct Log* log) __attribute__((nonnull(7, 8)));

int wrap_input_read_file(struct Str file_path,
                         struct Os* os,
                         struct Arena* perm,
                         struct Str* source_out,
                         struct Log* log) {
  memory_oom_set_job(S("read"), file_path, false);
  return os_read(os, file_path, SOURCE_LEN_MAX, perm, source_out, S(WRAP_INPUT_READ_FILE_OPERATION),
                 log);
}

enum WrapInputResult wrap_input_process_file(struct Str file_path,
                                             ptrdiff_t width,
                                             bool is_in_place,
                                             bool is_check,
                                             struct Os* os,
                                             struct Arena input,
                                             struct Arena scratch,
                                             struct Log* log) {
  struct Str source = {0};
  if (wrap_input_read_file(file_path, os, &input, &source, log) != 0) {
    return WRAP_INPUT_RESULT_ERROR;
  }

  const struct Str output_path = is_in_place ? file_path : S("");
  return process_source(source, width, false, file_path, output_path, is_check, os, input, scratch,
                        log);
}

enum WrapInputResult wrap_input_process_stdin(ptrdiff_t width,
                                              bool is_check,
                                              struct Os* os,
                                              struct Arena input,
                                              struct Arena scratch,
                                              struct Log* log) {
  const struct Str input_name = S("stdin");
  memory_oom_set_job(S("read"), input_name, true);
  struct Str source = {0};
  if (os_read_stdin(os, SOURCE_LEN_MAX, &input, &source, S("failed to read input"), log) != 0) {
    return WRAP_INPUT_RESULT_ERROR;
  }

  return process_source(source, width, true, input_name, S(""), is_check, os, input, scratch, log);
}

static enum WrapInputResult process_source(struct Str source,
                                           ptrdiff_t width,
                                           bool is_stream,
                                           struct Str input_name,
                                           struct Str output_path,
                                           bool is_check,
                                           struct Os* os,
                                           struct Arena input,
                                           struct Arena scratch,
                                           struct Log* log) {
  memory_oom_set_job(S("rewrap"), input_name, is_stream);
  // The output grows in place at the end of `input`, after the source, and nothing else allocates
  // there until it is emitted.
  struct Buf rewritten_source = {.arena = &input};
  rewrite_source(&rewritten_source, source, width, scratch);
  // Check mode only compares the output, so a check that runs out of memory after the rewrite is
  // still in it. The other modes name the write they go on to: back to the file, or to `stdout`.
  if (!is_check) {
    const struct Str write_phase = output_path.len > 0 ? S("write") : S("write rewritten");
    memory_oom_set_job(write_phase, input_name, is_stream);
  }
  return process_source_emit(source, buf_str(&rewritten_source), is_stream, input_name, output_path,
                             is_check, os, log);
}

static enum WrapInputResult process_source_emit(struct Str source,
                                                struct Str rewritten_source,
                                                bool is_stream,
                                                struct Str input_name,
                                                struct Str output_path,
                                                bool is_check,
                                                struct Os* os,
                                                struct Log* log) {
  const bool is_changed = !str_eq(rewritten_source, source);
  if (is_check) {
    if (!is_changed) {
      return WRAP_INPUT_RESULT_OK;
    }
    (void)log_error(log, "would rewrap '%.*s'", STR_FMT(input_name));
    return WRAP_INPUT_RESULT_CHANGED;
  }

  if (output_path.len > 0) {
    if (is_changed &&
        os_write(os, output_path, rewritten_source, S("failed to write file"), log) != 0) {
      return WRAP_INPUT_RESULT_ERROR;
    }
    return WRAP_INPUT_RESULT_OK;
  }

  if (os_print(os, 1, rewritten_source) != 0) {
    const int print_errno = errno;
    // It names the input whose output failed: `(for '<file>')` for a file and `(from '<stream>')`
    // for a stream. A closed pipe reaches here only when the caller ignored `SIGPIPE`. Under the
    // default disposition, the process dies of signal 13 inside the write.
    const char* reason = strerror(print_errno);
    if (is_stream) {
      (void)log_error(log, "failed to write rewritten input to 'stdout': %s (from '%.*s')", reason,
                      STR_FMT(input_name));
    } else {
      (void)log_error(log, "failed to write rewritten file to 'stdout': %s (for '%.*s')", reason,
                      STR_FMT(input_name));
    }
    return WRAP_INPUT_RESULT_ERROR;
  }
  return WRAP_INPUT_RESULT_OK;
}
