# Architecture

`cwrap` is a small C17 C comment formatter. It reads source files or standard input, refills the comment blocks that it can wrap, and writes the result to `stdout`, back to each input file, or as a `--check` report. This document describes the code: the directories and their dependency rules, the wrap pipeline, the job of each module, the conventions that every module follows, and the design decisions behind them. [README.md](README.md) covers usage and wrapping behavior, [BUILD.md](BUILD.md) covers the CMake build, and [STYLE.md](STYLE.md) covers the code style.

## Layout

- [src/app/](src/app/): The process entry point, the command-line interface, the command boundary, and the rewrap of one input.
- [src/domain/](src/domain/): Comment lexing, grouping, filling, Doxygen alignment, and source rewriting.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, and the out-of-memory report.
- [src/core/](src/core/): Small reusable primitives for parsing and ASCII classification.
- [tests/](tests/): The golden and idempotence test suites, the seeded random tests, the shared unit-test support library [tests/test_support.h](tests/test_support.h), and the in-memory host [tests/os_memory.h](tests/os_memory.h). Each C file under [tests/fixtures/](tests/fixtures/) has a matching file of expected output under [tests/expected/](tests/expected/). The idempotence test also rewraps these files and every first-party source file at several widths.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): The project entry point and the supported CMake configurations.
- [cmake/](cmake/): CMake profiles, quality tools, packaging, the golden and idempotence test drivers, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): The bundled dependencies `sharedstuff`, `copt`, and `acutest`.

### Dependency rules

First-party dependencies point inward:

- `app` can depend on every other directory.
- `domain` and `runtime` can depend on `core`.
- `core` depends on no other first-party directory.

A module can skip layers, and modules in the same directory can depend on each other. Vendored code is outside this hierarchy. A first-party include starts at the `src/` root, for example `#include "core/ascii.h"`, so every include names the directory that it depends on.

## Terms

These words have one meaning everywhere in this document:

- **Arena.** A `struct Arena` from `sharedstuff`. It has two pointers, `beg` and `end`, into one region. When the region is reserved and the arena commits pages on demand, it has a third pointer, `limit`. An arena never frees an individual allocation.
- **Perm.** An arena for data that lives longer than the function that makes it. A function takes it by pointer, as `struct Arena* perm`. `wrap_input_read_file` reads a source into a copy of the input arena, and takes this copy as its perm.
- **Scratch.** The run's arena for temporary data. A function takes it by value, as `struct Arena scratch`, so its allocations are free again when the function returns.
- **Input arena.** The run's arena for the source and the rewritten output of one input. A function takes a copy by value, as `struct Arena input`, so the next input reuses its memory.
- **Log.** A `struct Log`: a growable buffer with one line for each error, and an error count. A command prints its log once. A log grows in an arena of its own, and no callee gets this arena by value.
- **Block.** The one reservation that a command makes when it starts. The command splits the block into its arenas, and releases the block when it ends.
- **Comment block.** A group of comments that the rewrap refills as one unit, as a `struct CommentBlock`. A Doxygen block, a slash-star block and a starred block are kinds of comment block. The word "block" alone always means the reservation above.
- **Region.** A range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve.** To get address space from the system without memory behind it.
- **Commit.** To make reserved pages usable. The system charges committed pages against its memory limit.
- **Run.** One invocation of `cwrap` with the run action, which rewraps its inputs.
- **Input.** One source file, or standard input, that a run rewraps.

## Entry point and command

[main.c](src/app/main.c) defines `main`. `main` only calls `cli_dispatch` with the POSIX host, so the unit tests can link the code that maps an action to an exit code. These are the other modules in `src/app`:

- [cli](src/app/cli.h): `cli_parse` parses the command line into one of four actions: version, help, run, or error. It never prints and never exits. `cli.c` is the only translation unit that defines `COPT_IMPL`.
- [cli_dispatch](src/app/cli_dispatch.h): `cli_dispatch` calls `cli_parse`. For the run action, it calls `cmd_wrap_run`. For each other action, it prints one text through `os_print`: the diagnostic for the rejected command line, the `--help` text, or the `--version` text. It formats these texts in a reservation of its own, and releases that reservation when the command or the print returns. A command reserves its own memory.
- [cmd_wrap](src/app/cmd_wrap.h): `cmd_wrap_run` rewraps every input, as [Wrap pipeline](#wrap-pipeline) describes.
- [cwrap_version](src/app/cwrap_version.h): The version. CMake sets it when it configures the build tree.
- [exit_code](src/app/exit_code.h): Every command returns an `enum ExitCode`: `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).
- [wrap_input](src/app/wrap_input.h): Reads, rewrites, and emits one input, as [Wrap pipeline](#wrap-pipeline) describes.

## Wrap pipeline

A run has two layers:

- `cmd_wrap_run` is the outer boundary. Before it reads any input, it reserves the memory of the run once: the run's log, and one block of address space that it splits into the input arena and the scratch. It selects standard input or goes through the input paths in order, and gives each input to `wrap_input`. Check mode reports every input, and the default mode stops at the first input that fails. In-place mode checks every input before its first write, as [the decision below](#in-place-mode-checks-every-input-before-the-first-write) describes. `cmd_wrap_run` prints the run's log to `stderr` once, through `os_print`.
- `wrap_input` runs the read, rewrite, and emit steps for one input, in the input arena and the scratch that it gets from `cmd_wrap_run`. Its entry point for a file and its entry point for standard input share one rewrite path in memory. `WrapInputResult` keeps three results apart: success, a check-mode change, and an operational error. In the default mode, `wrap_input` prints each rewritten source to `stdout` in one `os_print` call. In in-place mode, it writes a file only when the file changes. In check mode, it writes nothing.

### Steps

`rewrite_source` ([src/domain/rewrite.h](src/domain/rewrite.h)) owns the domain pipeline for one source buffer. It copies a grouped slash-star block unchanged when this comment block is on one line, fits the column, and contains no tab. It runs every other comment block through the steps from Extract on:

1. **Lex** (`lex_comment_spans`): A character state machine records every comment span, and ignores strings and character literals. A shared transition keeps LF and CRLF line splices inside the state that they continue, including in a `//` comment. When a comment follows code, or follows the closer of another comment on its line, the step records the comment as trailing.
2. **Group** (`comment_group`): Each comment block records its exact opener marker: `//`, `///`, or `//!` for a line comment, and `/*`, `/**`, or `/*!` for a slash-star comment. The four markers other than `//` and `/*` make it a Doxygen block. Consecutive line comments with the same marker become one comment block when each comment is on the line after the previous comment, has only horizontal whitespace before it, and starts at the indent column of the first comment. So indented line comments group in the same way as line comments at the left margin. A line at a different indent, a code line, or a blank line starts a new comment block. A slash-star block is starred when a continuation `*` is at the decoration column (`opener + 1`). A `*` under the hanging prose is a list marker. The step leaves out three kinds of comment:
   - A trailing comment.
   - A `//` comment that a backslash line splice continues. Its spliced physical lines have no `//` marker for a refill to restore, so the step copies the comment unchanged, and the comment ends the comment block before it.
   - A comment block with no letter, digit, or non-ASCII byte, such as `/**/` or a row of stars. It keeps its exact bytes.
3. **Extract** (`comment_extract_body`): The step strips the decoration (`*/` before `*`) and classifies each payload line. It drops a last line that holds only the closer, and strips the hanging indent. Extra indent that is not a tag continuation makes an indented sample. A line whose payload starts with a tool directive is a directive line in any comment shape. The directives are `cppcheck-suppress` with any suffix, `NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN`, `NOLINTEND`, `clang-format off`, `clang-format on`, and `IWYU pragma:`, each as a whole word. `tool_directives` in [src/domain/comment.c](src/domain/comment.c) lists them. Fill copies a directive line without wrapping it, so the directive line ends the paragraph before it, and the next line starts a new paragraph. In a Doxygen block, a line that starts with any `@word` or `\word` command is a tag that starts a paragraph. A prose line after the tag, at any indent, continues the tag, and the step removes the indent of that line. The step does not change `@code` through `@endcode` and `@verbatim` through `@endverbatim`, in either spelling, in the same way as a Markdown fence.
4. **Align** (`doxygen_align_parameters`): The step aligns the descriptions of all `@param` tags in a comment block in one column. It aligns the descriptions of the `@retval` tags separately, in the same way.
5. **Fill** (`fill_body_lines`): The step fills prose paragraphs greedily with the atoms of `atom_split`. A tag keeps its keyword on the first line, and a `@param` or `@retval` tag also keeps its name there. The wrapped description of a tag hangs under the first word of the description. A Doxygen command, a list marker, or a tool directive inside a paragraph stays on the line of the word before it. When the body of a starred block fits on the opener line, the step fills the body again with room for the closer, which then follows the body on that line.
6. **Emit** (`comment_emit`): The step restores the recorded opener, prefixes, and closers, with the original LF or CRLF convention of the comment block.
7. **Splice**: The rewrite copies the bytes outside comments unchanged.

### Fixed-point invariant

Rewrapping is idempotent: a second rewrap of the output of a first rewrap changes nothing, and `--check` agrees. `--check` works as a CI gate only because of this property. If `comment_emit` wrote a shape that it cannot make again from its own output, `--check` would report a rewrap for a file that `--in-place` just wrote.

The constraint is on the emitter. Every closer, prefix, and indent that the emitter writes must parse again to the same shape of comment block:

- A starred block whose refilled prose fits on the opener line puts its closer on that line, because a closer alone on its line would parse again as a line to join.
- A wrapped tag description parses again as a continuation of that tag at any indent, so it refills at any width.
- No refilled continuation line starts with a Doxygen command, a list marker, or a tool directive, because such a line would parse again as a new tag, list item, or directive line.

### Errors

No domain or runtime module writes a failure diagnostic to `stderr` itself. The one exception is the out-of-memory report, which `os_oom` prints through `memory_oom_report` before the process exits. Every fallible function records its errors in the log that its caller gives, and the command boundary prints the log once. The domain cannot fail, so every error is about an input, never about a comment.

## Modules

Each item below links to the module's header, which documents its interface. The `src/app` modules are in [Entry point and command](#entry-point-and-command).

### Domain (`src/domain`)

- [atom](src/domain/atom.h): Measures display-column width by Unicode 16.0, and splits prose into atoms, the units that a wrap never breaks.
- [comment](src/domain/comment.h): Groups comment spans into comment blocks, extracts the classified body lines, and rebuilds the selected comment shape.
- [doxygen](src/domain/doxygen.h): Parses a Doxygen command at the start of a payload, and aligns the `@param` and `@retval` descriptions.
- [fill](src/domain/fill.h): Fills prose paragraphs greedily, and keeps samples, lists, tags, and hanging indentation.
- [lex](src/domain/lex.h): Finds comments, and does not treat string literals or character literals as comments. It records source spans, line positions, indentation, and whether a comment follows code.
- [rewrite](src/domain/rewrite.h): Runs the domain pipeline over one source buffer, and splices the rewritten comments between the unchanged source ranges.

### Runtime (`src/runtime`)

- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, in an object library of its own, `cwrap_os_posix`. The `cwrap` executable and every unit test that is registered without `OS memory` link it, as [BUILD.md](BUILD.md#unit-tests) lists. Its `struct Os` has no state.
- [memory](src/runtime/memory.h): Prints the out-of-memory report: the command's log, and then the phase and the input in which the run ran out of memory. `memory_oom_set_job` takes a phase, an input name, and whether the input is a stream, because the report names the phase and the run has no jobs.
- [os](src/runtime/os.h): The host interface. Every read, write and print that the program does goes through `os_read`, `os_read_stdin`, `os_write` and `os_print`. Each function that accesses the host takes a `struct Os*`.
  - `os_read` opens a path without blocking, and inspects the open descriptor. It takes a size limit from each caller, and checks it before any read or allocation. It rejects a file that is not a regular file, a file that contains a `NUL`, and a file that changes size during the read. It returns the bytes in the caller's arena.
  - `os_read_stdin` reads standard input to its end, up to a size limit, and does not close it. It rejects input that contains a `NUL`.
  - `os_write` rewrites a user's source, so the write is atomic. It `fsync`s and closes a temporary file next to the destination, and then renames the temporary file over the destination, so a failure leaves an existing file as it was. An existing destination keeps its permission bits, and a new file gets mode `0666` reduced by the process umask.
  - `os_print` carries every byte that the program prints: the rewritten source in the default mode, the log at the end of a run, a rejected command line, `--help` and `--version`, so a test on the in-memory host sees all of it. One call writes its text as a whole. It retries a short write, in writes of at most 1 GiB, because macOS refuses a count above `INT_MAX`.
  - `os_oom` is the one other member. `sharedstuff` declares it, and each program defines it.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [parse](src/core/parse.h): Parsing of text into scalar values.

## Cross-cutting conventions

Every module follows these rules. When a design decision is behind a rule, the link goes to its reasons and costs.

### Memory

- Every first-party allocation comes from an arena. `cwrap` has three arenas: the log, which holds the errors of the run; the input arena, which holds the source and the rewritten output of one input; and the scratch, which holds the temporary data of one input. `os_read` and `os_read_stdin` return their result in the caller's arena. See [Memory: arenas only](#memory-arenas-only) and [Memory: a log, an input arena and a scratch, and no pool](#memory-a-log-an-input-arena-and-a-scratch-and-no-pool).
- A command reserves its memory once, when it starts: the log at its full size, and the input arena and the scratch as one block, through `arena_reserve_halving`. Each arena has a fixed size, which the input limit and the measured shapes bound, as its definition in `cmd_wrap.c` states. Each input gets the input arena and the scratch by value, so the next input reuses their memory. An arena commits pages only when its allocations get to them, and then commits one mebibyte more than the allocation needs. Only committed pages count against the system's memory. See [Memory: fixed sizes, reserved, then committed on demand](#memory-fixed-sizes-reserved-then-committed-on-demand).
- An allocation that does not fit calls `os_oom`, which reports and exits, so no call site checks for allocation failure, and the domain cannot fail. See [Out of memory exits the process](#out-of-memory-exits-the-process).
- Under the address sanitizer, two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`. See [Address sanitizer coverage in arenas](#address-sanitizer-coverage-in-arenas).
- A log's arena is never an arena that a callee gets by value, so the log never grows into bytes that a callee uses. See [A log must not grow in a borrowed arena](#a-log-must-not-grow-in-a-borrowed-arena).

### One thread

`cwrap` runs on one thread and rewraps one input at a time, so two inputs never use an arena or a log at the same time. See [Memory: a log, an input arena and a scratch, and no pool](#memory-a-log-an-input-arena-and-a-scratch-and-no-pool).

### Strings and text input

Every first-party string is a counted `struct Str` with a signed `ptrdiff_t` length, and every length, count and column is a `ptrdiff_t`. Each command-line argument becomes a view where the code parses it or gives it to `wrap_input`. The lexer and the wrapping pipeline use views into the source, and a refilled line is new text in the scratch. See [Counted strings with signed lengths](#counted-strings-with-signed-lengths).

No input format gives a meaning to a `NUL` byte, so the input checks stop a `NUL` before it gets into the rewritten output. `os_read` and `os_read_stdin` reject input that contains a `NUL`, and `os_read` reports this policy failure separately from system errors. A path that contains a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read the path only up to that byte.

### Errors and diagnostics

- A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log`. It records every error that it finds in that log, and never truncates one. A message is one `printf` format that the compiler checks. See [A growable log, and every error in one run](#a-growable-log-and-every-error-in-one-run) and [Log messages use checked format strings](#log-messages-use-checked-format-strings).
- A filesystem or stream action also takes the `operation` of the caller, such as `failed to read file`, and logs the whole message, because only the action knows the cause.
- An action, and a producer that can fail, return `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, for example a `struct Str` by value. A predicate returns `bool`. Running out of memory is never a return value. An operation with a third result that is not an error returns an enum. For example, `wrap_input` returns a `WrapInputResult`, which keeps a check-mode change apart from an operational error. [STYLE.md](STYLE.md#function-signatures) gives the full rules.
- A first-party diagnostic is lowercase prose. It names the failed operation first, puts literal names and values in single quotes, and puts a value after the cause. Messages from the system or from a vendored library use their original capitalization and punctuation. [STYLE.md](STYLE.md#errors-and-diagnostics) gives the full rules.

### Input limits

Each caller of `os_read` gives a limit for the kind of file that it reads. The read checks the limit before it allocates, so it never loads a file that is too large. `SOURCE_LEN_MAX` in `wrap_input` is 64 MiB for each source file and for standard input. `os_read_stdin` has no size to check before it reads, so it stops one byte past the limit and rejects the input.

### Host access

No code in `src/` outside `runtime/fs.c` makes a system call on a path or a standard stream, or prints through `stdio`. All host access goes through `os.h`, and the link step selects the host for each executable. A new unit test links the in-memory host, unless it needs a fact that only a real filesystem has. See [The `os` interface](#the-os-interface).

### Line endings

Each comment block records whether its source uses LF or CRLF. Refilled lines use the same convention, and the source bytes outside the refilled lines do not change.

## Design decisions

Each decision below states what the code does and why, and some decisions also state what they cost. Read the decision before you change one of these designs, and make sure that the change deals with each of its reasons. The decisions come in groups, in this order: memory, strings, logs and their messages, host access, tests, and where the shared code lives.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas, and the code never frees an individual allocation. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment needs to say which code frees a buffer.
- The bounds check is one comparison, and the comparison cannot overflow.
- A `Buf` can grow in place when it is the last allocation. The rewritten output grows after its source in the input arena, and standard input grows in place while the run reads it.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a log, an input arena and a scratch, and no pool

**Decision.** `cwrap` runs on one thread and processes one input at a time, in three arenas:

- **The log.** It holds the errors of the run: at most one or two diagnostics for each input, and each diagnostic quotes a path from the command line. `ARG_MAX` limits the inputs to about 600,000 on Linux, so the log uses less than 40 MB of its 64 MiB, even when every path is one byte. Its size depends on the number of inputs, not on their size, so it keeps its full size under an address-space limit.
- **The input arena.** It holds one source and its rewritten output.
- **The scratch.** It holds the temporary data of one input. `rewrite_source` keeps the comment spans and the comment blocks there, and gives each comment block its own copy of the scratch by value.

`cmd_wrap_run` reserves the log, and then the input arena and the scratch in one block, when it starts. Each input gets the input arena and the scratch by value, so the next input uses the same memory again. `cli_dispatch` formats the diagnostic for the command line and the help text in a 16 MiB reservation of its own, which is several times the largest `ARG_MAX` in use. It releases this reservation when the run or the print returns.

**Reasons.**

- The run needs memory for its largest input, not for the sum of its inputs.
- The lines and atoms of one comment block are free again before the next comment block.
- The log grows while each input copies the input arena and the scratch by value, so the log needs an arena that no input gets.
- A diagnostic quotes an argument as a whole, and only the system's limit on argument size bounds an argument, so the diagnostic for the command line goes in a reservation that is several times that limit.
- The reservation for the command line has a fixed size, and does not take its size from `sysconf(_SC_ARG_MAX)`. glibc derives that value from `RLIMIT_STACK`, so the value is very large when the stack is unlimited, and musl gives a different value.
- `cwrap` runs on one thread, because there is no reason to rewrap inputs in parallel. Also, `os_write` reads the umask by setting it and restoring it, and a diagnostic calls `strerror`. Both are safe only with one thread.

### Memory: fixed sizes, reserved, then committed on demand

**Decision.**

1. When the run starts, it reserves its memory with `PROT_NONE`, and it releases the memory when it ends: the log (`CMD_WRAP_LOG_LEN`, 64 MiB) on its own, and one block for the input arena (`CMD_WRAP_INPUT_LEN`, 8 GiB) and the scratch (`CMD_WRAP_SCRATCH_LEN`, 8 GiB). The reservations cost no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part, so the fast path is still one bounds check. The slow path commits pages to one mebibyte past what the allocation needs, but not past the end of the region.
3. The sizes are fixed. They come from the input limit and from the measured shapes, not from a prediction for the inputs:
   - The output and the temporary data grow linearly with the source for every shape except two. The worst linear shape that was measured took less than 90 bytes of scratch for each source byte, which is less than 6 GiB at `SOURCE_LEN_MAX`.
   - A comment indented by about half its line writes an output that grows with the square of the source: 2.45 GB for a 140 KB file at `-w 20`.
   - A Doxygen `@param` with a long name at a narrow width grows both the output and the scratch. A 60,033-byte file at `-w 1` writes 400 MB, and takes about 6,685 bytes of scratch for each source byte.
   - A buffer at most doubles past what it holds, so the input arena holds an output of up to 4 GiB.
4. If the system refuses the block, for example under a `ulimit -v` below the size of the block, `arena_reserve_halving` halves the block, and both arenas with it, up to six times. It then halves the first size that fits one more time, so that the rest of the process has room. If the system refuses even the smallest size, the run ends with the out-of-memory report. A run under a limit of a few GiB still handles ordinary inputs. An input that grows past the smaller arenas runs out of memory, with the report below.

**Reasons.**

- A fixed size needs no pass over the inputs before the first read, no budget, and no refusal messages.
- On macOS, a reservation costs system time in proportion to its size: about 1 µs per GiB to reserve, and the same to release. The arena sizes come from their bounds, not from the largest size that the system allows, because 65 GiB would cost a very small run about 0.2 ms, 7% of its time.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under strict overcommit.

**Cost.** Under a `ulimit -v` below the reserved total, the arenas are smaller, and an input that grows past them runs out of memory, where a `malloc` design could have fitted it. An output larger than 4 GiB runs out of memory even when no limit is set. The two shapes that grow with the square of the source, which `cmd_wrap.c` describes, get to that size, or to the size of the scratch, from a few hundred kilobytes of source at a narrow width.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`, and the `cwrap` version, in `main.c`, calls `memory_oom_report`, which does these steps:

1. It prints the errors in the command's log. `cmd_wrap_run` names its log with `memory_oom_set_log`.
2. It prints a line in the shape of the other diagnostics, with the phase and the input: `failed to rewrap file: out of memory ('<path>')`, `failed to read input: out of memory (from 'stdin')`, or `failed to run: out of memory` outside an input. Before each phase of an input, `wrap_input` names the phase and the input with `memory_oom_set_job`.

Then `os_oom` exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `cwrap` is a CLI tool, and its in-place write is atomic, so each file that a stopped run leaves is either as it was or completely rewritten.
- No call site checks for allocation failure, and the domain cannot fail.
- The report names the input that the run was working on. A user needs that name to find the input that failed. The report has no program-name prefix, because no other message of `cwrap` has one.

**Vendored code.** No vendored library in the program allocates. `copt` parses the arguments in place.

### Address sanitizer coverage in arenas

**Decision.** When `cwrap` is compiled with the address sanitizer, an arena over a reserved region poisons the pages that it commits, and `arena_alloc` unpoisons only the bytes that it returns. Such an arena leaves a poisoned gap before each allocation, so two of its allocations are never adjacent, and code that extends the last allocation in place uses `arena_extend`. An arena over memory that the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without the poisoning, the debug preset loses most of its overflow checks.

**Limits.**

- The arena does not poison scratch memory again when a function returns, so ASan does not find a use of scratch memory after the function that owned it returned.
- `arena_release` leaves the shadow memory to the sanitizer's `munmap` interceptor, which clears it, so a later mapping at the same addresses gets no old poison. Unpoisoning the whole region instead would write an eighth of each reservation as resident shadow memory.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str` from [str.h](vendor/sharedstuff/include/shared/str.h): a pointer and a `ptrdiff_t` length, for both owned and borrowed text. Every length, count and column is a `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to such a call. Each such change asserts that the value is not negative, unless this is true by construction. A C string exists only where libc, POSIX, or a vendored API needs one. The POSIX host copies each path into a `PATH_MAX` buffer at the system call, with `copy_cstr`.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text, and a body line is a view into the source until a refill makes new text.

**Deliberate.** The `NUL` checks in [Strings and text input](#strings-and-text-input) are a product decision, and the string type does not force them. If the product decision changes, you can remove the `memchr` checks in both hosts and the contract tests that pin them, `test_read_rejects_embedded_nul` and `test_read_stdin_rejects_embedded_nul_and_input_over_limit`.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter. The log holds every error, with no truncation.
- Check mode and in-place mode report every bad input in one run, and the default mode stops at the first bad input, as the in-place decision above describes.
- A rejected command line reports only its first error, because the later errors are usually results of the first error.

**Reasons.**

- A user sees every error in one run.
- No message buffer has a fixed size, so no message is truncated, and no message must be early enough in the order to survive truncation.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` live in an arena that no callee gets by value. The run gives its log an arena of its own.

**Reason.** A `Buf` grows in place. If a callee gets a copy of the same arena by value, the callee allocates the same bytes that the log grows into. The two then write over each other, with no crash and no ASan report.

[STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of the rule as a bug, not as a style problem.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`, and an integer prints with the conversion for its type. For example:

```c
log_error(log, "failed to read file: %s ('%.*s')", reason, STR_FMT(path));
```

**Reasons.**

- Each message is one sentence in the source, so a reader reads its wording on one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- Each first-party function that formats a message has the attribute, so its callers get the same check.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue, and put a computed string in a variable first.
- A string longer than `INT_MAX` bytes causes an assertion failure at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates the text, and writes it with a second `vsnprintf`. These functions are `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. Only those functions change the measured `int` length to a `ptrdiff_t`.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. If it did, the Clang analyzer, version 22, would report a false `va_end` on an uninitialized list when it targets AArch64 Linux, where a `va_list` is a structure. Instead, the function starts its list once for each `vsnprintf` pass, as `buf_printf` does. `record_error` in `cli.c`, and `test_log_is` and `test_text_is` in `tests/test_support.c`, follow this rule.
- `%.*s` stops at a `NUL` byte. The input checks reject a `NUL` in file content, on standard input, and in paths at the `os` boundary, and a message quotes only the command-line arguments and the name `stdin`, so a message gets no `NUL` from input. The `os` boundary's `EINVAL` message quotes a path that contains a `NUL` only up to that byte, and the contract test for this case shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that is longer than the column limit, so some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`: `os_read`, `os_read_stdin`, `os_write`, `os_print`, and `os_oom`. `fs.c` is the POSIX implementation. [tests/os_memory.c](tests/os_memory.c) is an in-memory implementation for unit tests: a set of files by path with a standard input, injected failures, and captured output. The link step selects one implementation for each executable. A function that accesses the host takes `struct Os*` just before its arenas, and an `os_*` function takes it first.

**Reasons.**

- The tests of the command, of each input and of the out-of-memory report need no temporary files and no redirected descriptors. The in-memory host also injects the failures that are hard to cause on POSIX, such as a failed write to `stdout` or a failed in-place write.
- `os_print` carries every byte that the program prints, `--help` and `--version` included, so a test sees all of it, and the code reports a failed write where it occurs, with its input.
- Host access is visible at one boundary, and is not spread over calls to `stdio` in the app layer.
- A link-time choice needs no table of function pointers, because no executable needs two hosts.

**Linking.** `cwrap` and every unit test registered without `OS memory` link `runtime/fs.c`. The tests registered with `OS memory` link `tests/os_memory.c`, as [BUILD.md](BUILD.md#unit-tests) lists.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. `src/runtime/test_os.c` compiles once against each host and runs the same cases, and both executables must pass, so every property that a test relies on in the in-memory host is also true on POSIX. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file where POSIX returns `ENOTDIR`, and a contract test checks this case.

**The contract is `cwrap`'s own.** `os_read` opens a path and does not call `stat`, because `stat` does not see that the process may not read a file. `os_write` rewrites a user's source, so the write is atomic and synced. It writes, `fsync`s and closes a temporary file next to the destination, and then renames the temporary file over the destination. The in-memory host has no directory tree, no symlinks and no path resolution, because `cwrap` takes paths and never lists a directory. The POSIX-only tests in `test_fs.c` cover these cases.

### In-place mode checks every input before the first write

**Decision.** In in-place mode, a first pass without writes reads every path with `wrap_input_read_file`. It logs, in path order and in the words of `os_read`, each path that meets one of these conditions. The path:

- is missing
- cannot be read
- is not a regular file
- is larger than `SOURCE_LEN_MAX`
- contains a `NUL`

If the log has an error after that pass, the run writes no file. The default mode reads each file once, in order, and stops at the first bad path, after it prints the files before that path. Check mode writes nothing, reads each file once, and reports every input in path order.

**Reasons.**

- A bad input among many leaves every file as it was, and one run reports every bad input. The default mode only prints, and the user can run it again, so it keeps one read of each file.
- The pass costs a second open and read of every file in in-place mode, and the one `fsync` for each written file hides this cost.
- A run that has an error writes nothing, unlike `clang-format -i`, which rewrites the good inputs. The user fixes the bad input and runs the same command again.

**Limit.** A write that fails, or a file that changes during the run, still stops the run part-way, after the run wrote the inputs before it. Each write is atomic, so no file is left half-written. When a file is given twice, the run writes it once, at its first position, and finds it already wrapped at its second position.

### Seeded random tests in the normal suite

**Decision.** `tests/test_random.c` checks two properties on generated files. A second rewrap changes nothing, and the bytes outside comments do not change, except for the line breaks that a refill moves between the lines of one `//` comment block. The test gets the files from the generator in `tests/test_support`, a 64-bit linear congruential step, with a fixed seed (`TEST_RNG_SEED_DEFAULT`), so the test checks the same cases every time. A failed check names the case.

**Reasons.**

- The idempotence suite checks the [fixed-point invariant](#fixed-point-invariant) only on fixed files. The random tests check it on files that nobody wrote by hand.
- The random tests run in every test preset, under its sanitizers, with no fuzzing engine.
- A fixed seed makes the suite deterministic, so a failure occurs every time the test runs.

**Limit.** The random tests reach only the input shapes that their generator lists. A fuzzing harness would search past these shapes, and the tree does not have one yet.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`, and has its own `os` interface and its own arena layout.

**Reason.** These parts do not depend on what a program does.
