#include "runtime/memory.h"

#include <stdbool.h>
#include <stddef.h>

#include "runtime/os.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/** The command's log, which the out-of-memory report prints first, or `NULL`. */
static const struct Log* memory_oom_log;

/** Phase the program is in, for the out-of-memory report, or empty outside an input. */
static struct Str memory_oom_phase;

/** Name of the input `memory_oom_phase` works on. */
static struct Str memory_oom_input_name;

/** Whether `memory_oom_input_name` names a stream rather than a file. */
static bool memory_oom_is_stream;

void memory_oom_set_log(const struct Log* log) {
  memory_oom_log = log;
}

void memory_oom_set_job(struct Str phase, struct Str input_name, bool is_stream) {
  memory_oom_phase = phase;
  memory_oom_input_name = input_name;
  memory_oom_is_stream = is_stream;
}

void memory_oom_report(struct Os* os) {
  // Each piece is printed as it is, because building one message would need memory. A print that
  // fails cannot be reported either, so the results are discarded.
  if (memory_oom_log != NULL) {
    (void)os_print(os, 2, buf_str(&memory_oom_log->buf));
  }
  if (memory_oom_phase.len == 0) {
    (void)os_print(os, 2, S("failed to run: out of memory\n"));
    return;
  }
  (void)os_print(os, 2, S("failed to "));
  (void)os_print(os, 2, memory_oom_phase);
  (void)os_print(
      os, 2,
      memory_oom_is_stream ? S(" input: out of memory (from '") : S(" file: out of memory ('"));
  (void)os_print(os, 2, memory_oom_input_name);
  (void)os_print(os, 2, S("')\n"));
}
