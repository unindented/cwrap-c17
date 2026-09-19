#include "app/cmd_wrap.h"

#include <errno.h>
#include <stdio.h>

#include "app/wrap_input.h"
#include "core/error.h"
#include "shared/string_buffer.h"

enum ExitCode cmd_wrap_run(const struct WrapOptions* options) {
  struct StringBuffer error_buffer;
  string_buffer_init(&error_buffer);

  enum ExitCode rc = EXIT_CODE_FAILURE;
  bool has_failed = false;
  if (options->is_stdin) {
    const enum WrapInputResult result =
        wrap_input_process_stream(stdin, "stdin", options->width, options->is_check, &error_buffer);
    switch (result) {
      case WRAP_INPUT_RESULT_OK:
        break;
      case WRAP_INPUT_RESULT_CHANGED:
      case WRAP_INPUT_RESULT_ERROR:
        has_failed = true;
        break;
    }
  } else {
    for (int i = 0; i < options->path_count; i++) {
      const enum WrapInputResult result =
          wrap_input_process_file(options->paths[i], options->width, options->is_in_place,
                                  options->is_check, &error_buffer);
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
  }
  if (has_failed) {
    // `error_buffer` is empty only when recording the diagnostic itself ran out of memory. The
    // fallback keeps that failure visible instead of reporting a silent non-zero exit.
    fprintf(stderr, "%s\n", error_buffer.data != NULL ? error_buffer.data : "wrap failed");
    goto cleanup;
  }
  if (!options->is_check && !options->is_in_place && fflush(stdout) != 0) {
    // Relay the stream's own reason. A full disk and a closed or invalid descriptor call for
    // different responses, and without this they are indistinguishable. A closed pipe reaches here
    // only when the caller ignored `SIGPIPE`. Under the default disposition, the process dies of
    // signal 13 inside the flush instead. `fflush` sets `errno`, so reading it here is valid.
    char error_message[ERROR_MESSAGE_SIZE];
    fprintf(stderr, "failed to write rewritten %s to 'stdout': %s\n",
            options->is_stdin ? "input" : "file",
            error_system_message(error_message, sizeof(error_message), errno));
    goto cleanup;
  }
  rc = EXIT_CODE_OK;

cleanup:
  string_buffer_free(&error_buffer);
  return rc;
}
