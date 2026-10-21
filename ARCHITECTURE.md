# Architecture

`cwrap` is a small C17 C comment formatter. It reads source files or standard input, refills wrappable comment blocks, and writes the result to `stdout`, in place, or as a `--check` report. This document describes the code structure. See [README.md](README.md) for usage and wrapping behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundary.
- [src/domain/](src/domain/): Comment lexing, grouping, filling, Doxygen alignment, and source rewriting.
- [src/runtime/](src/runtime/): The host interface and its POSIX implementation, and the out-of-memory report.
- [src/core/](src/core/): Small reusable primitives for parsing and ASCII classification.
- [tests/](tests/): Golden and idempotence test suites, the shared unit-test support library ([tests/test_support.h](tests/test_support.h)), and the in-memory host ([tests/os_memory.h](tests/os_memory.h)). Each C file under [tests/fixtures/](tests/fixtures/) has a matching expected output under [tests/expected/](tests/expected/). The idempotence test also rewraps these files and every first-party source file at several widths.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden and idempotence test drivers, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`sharedstuff`, `copt`, `acutest`).

First-party dependencies point inward:

- `app` can depend on `domain`, `runtime`, and `core`.
- `domain` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/ascii.h"`. This convention makes each dependency visible at the call site. Vendored support is outside the first-party hierarchy.

## Entry point and command

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)) on the POSIX host. This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls `cmd_wrap_run` ([src/app/cmd_wrap.h](src/app/cmd_wrap.h)).

`cli.c` is the one translation unit that defines `COPT_IMPL`. `cli_parse` never prints or exits. `cli_dispatch` prints a rejected command line's diagnostic, `--help` and `--version` through `os_print`, and formats each in a reservation of its own, which it releases once the command or the print returns. A command reserves its own memory. Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

The command has two layers:

- `cmd_wrap_run` is the outer boundary. It reserves the run's memory once, before it reads any input: the run's log, and one block of address space it splits into an input arena for one input's source and rewritten output and a scratch for temporaries. It selects standard input or iterates through the input paths, and delegates each input to [wrap_input](src/app/wrap_input.h). Check mode reports every input, the default mode stops at the first input that fails, and in-place mode checks every input before its first write, as [the decision below](#in-place-mode-checks-every-input-before-the-first-write) describes. The boundary prints the run's log to `stderr` exactly once, through `os_print`.
- `wrap_input` runs the read, rewrite, and emit steps for one input in the input arena and scratch `cmd_wrap_run` passes it. Its file and standard-input entry points share the same in-memory rewrite path. `WrapInputResult` distinguishes success, a check-mode change, and an operational error. The module prints each rewritten source to `stdout` in one `os_print` call in the default mode, writes only a changed file in in-place mode, and writes nothing in check mode.

No domain or runtime module writes a failure diagnostic directly to `stderr`, apart from the out-of-memory report, which `os_oom` prints through `memory_oom_report` on the way out of the process. Every fallible function records its errors in a `struct Log`, a growable buffer with one line per error and an error count, which the command boundary prints once. The domain cannot fail, so every error concerns an input, never a comment.

## Wrap pipeline

`rewrite_source` ([src/domain/rewrite.h](src/domain/rewrite.h)) owns the domain pipeline for one source buffer:

1. **Lex** (`lex_comment_spans`): A character state machine records every comment span and ignores strings and character literals. A shared transition keeps LF and CRLF line splices inside the state they continue, including a `//` comment. A comment that follows code or the closer of another comment on its line is recorded as trailing.
2. **Group** (`comment_group`): Each block records its exact opener marker: `//`, `///`, or `//!` for a line comment, and `/*`, `/**`, or `/*!` for a block comment. The four markers other than `//` and `/*` make it a Doxygen block. Consecutive line comments with the same marker become one block when each sits on the line after the last with only horizontal whitespace before it, at the indent column of the first, so an indented run groups like a top-level one. A line at another indent, a code line, or a blank line starts a new block. Trailing comments are omitted. So is a `//` comment continued by a backslash line splice: its spliced physical lines carry no `//` marker for a refill to restore, so it is copied unchanged and ends the run before it. So is a block with no letter, digit, or non-ASCII byte, such as `/**/` or a row of stars, which keeps its exact bytes. A slash-star block is starred when a continuation `*` sits at the decoration column (`opener + 1`). A `*` under the hanging prose is a list marker.
3. **Extract** (`comment_extract_body`): Decoration is stripped (`*/` before `*`) and each payload line is classified. A closer-only last line is dropped. Hanging indent is stripped; extra indent that is not a tag continuation is an indented sample. A line whose payload starts with a tool directive (`cppcheck-suppress` with any suffix, `NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN`, `NOLINTEND`, `clang-format off`, `clang-format on`, or `IWYU pragma:`, each as a whole word) is a directive line in any comment shape: fill copies it unwrapped, so it ends the paragraph before it and the next line starts a new one. The list lives in `tool_directives` in [src/domain/comment.c](src/domain/comment.c). In a Doxygen block, a line that starts with any `@word` or `\word` command is a tag that starts a paragraph, and a prose line after it, indented at any depth, continues it with the indent removed. `@code` through `@endcode` and `@verbatim` through `@endverbatim`, in either spelling, are left untouched like a Markdown fence.
4. **Align** (`doxygen_align_parameters`): `@param` descriptions share a column, and so do `@retval` descriptions.
5. **Fill** (`fill_body_lines`): Prose paragraphs are greedily filled with [STYLE.md](STYLE.md) atoms. A tag keeps its keyword, and a `@param` or `@retval` its name, on the first line, and its wrapped description hangs under the first description word. A Doxygen command, list marker, or tool directive inside a paragraph travels with the word before it. A starred body that collapses onto the opener line is filled again with room for the closer that then trails it.
6. **Emit** (`comment_emit`): The recorded opener, prefixes, and closers are restored with the block's original LF or CRLF convention.
7. **Splice**: Non-comment bytes are copied unchanged.

## Modules

### Domain (`src/domain`)

- [lex](src/domain/lex.h): Finds comments without treating string literals or character literals as comments. It records source spans, line positions, indentation, and whether a comment trails code.
- [comment](src/domain/comment.h): Groups comment spans into blocks, extracts classified body lines, and reconstructs the selected comment shape.
- [atom](src/domain/atom.h): Measures Unicode 16.0 display-column width and splits prose into unbreakable wrapping atoms.
- [fill](src/domain/fill.h): Greedily fills prose paragraphs while preserving samples, lists, tags, and hanging indentation.
- [doxygen](src/domain/doxygen.h): Parses a Doxygen command at the start of a payload and aligns `@param` and `@retval` descriptions.
- [rewrite](src/domain/rewrite.h): Runs the domain pipeline over one source buffer and splices rewritten comments between unchanged source ranges.

### App (`src/app`)

- [wrap_input](src/app/wrap_input.h): Coordinates file or stream input, source rewriting, and the selected output mode.

### Runtime (`src/runtime`)

- [os](src/runtime/os.h): The host interface. Every read, write and print the program performs goes through `os_read`, `os_read_stdin`, `os_write` and `os_print`, and each function that reaches the host takes a `struct Os*`. `os_read` opens a path without blocking, inspects the opened descriptor, and takes a size limit from each caller, which it checks before any read or allocation. It rejects a file that is not regular, one holding a `NUL`, and one that changes size during the read, and returns the bytes in the caller's arena. `os_read_stdin` reads standard input through its end without closing it, up to a size limit, and rejects one holding a `NUL`. `os_write` rewrites a user's source, so it is atomic: it `fsync`s and closes a sibling temporary file before renaming it over the destination, preserving an existing file on failure. An existing destination keeps its permission bits. A new file gets mode `0666` reduced by the process umask. `os_print` carries every byte the program prints: the rewritten source in the default mode, the log a run ends with, a rejected command line, and `--help` and `--version`, so a test on the in-memory host sees all of it. One call writes its text whole, retrying a short write, in writes of at most 1 GiB, because macOS refuses a count above `INT_MAX`. `os_oom` is the remaining member, declared by `sharedstuff` and defined by each program.
- [fs](src/runtime/fs.h): The POSIX implementation of `os.h`, an object library of its own (`cwrap_os_posix`). The `cwrap` executable and every unit test registered without `OS memory` link it. Its `struct Os` has no state.
- [memory](src/runtime/memory.h): Prints the out-of-memory report: the command's log, then the phase and the input the run ran out in. `memory_oom_set_job` takes a phase, an input name and whether the input is a stream, because the report names the phase and the run has no jobs.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [parse](src/core/parse.h): Parsing of text into scalar values.

## Fixed-point invariant

Rewrapping is idempotent: a second pass over the output of a first must change nothing, and `--check` must agree. This is what lets `--check` serve as a CI gate, since a shape `comment_emit` cannot reproduce from its own output would report a rewrap for a file `--in-place` had just written. The constraint falls on the emitter: every closer, prefix, and indent it writes has to re-parse to the same block shape. A starred block whose refilled prose fits on the opener line therefore trails its closer rather than orphaning it on a line of its own, because a lone closer would re-parse as a line to join. A wrapped tag description re-parses as that tag's continuation whatever its indent, so it refills at any width. No refilled continuation line starts with a Doxygen command, a list marker, or a tool directive, which would re-parse as a new tag, list item, or directive line.

## Cross-cutting conventions

- **Memory**: Every first-party allocation comes from an arena, a `{beg, end, limit}` triple from `sharedstuff` that frees nothing piecemeal. `cwrap`'s arenas are its log, its input arena and its scratch: the input arena holds one input's source and rewritten output, the scratch its temporaries, and the log the run's errors, and no callee gets the log's arena by value. A command reserves its memory once, when it starts: the log at its full size, and the input arena and the scratch as one block, through `arena_reserve_halving`, each of a fixed size that the input limit and the shapes measured bound, as its definition in `cmd_wrap.c` states. Each input gets the input arena and the scratch by value, as `input` and `scratch`, so the next input reuses their memory. A reservation costs address space only: an arena commits pages as it fills, so only the pages a run uses count against the system's memory. An allocation that does not fit calls `os_oom`. In `cwrap` it prints the log so far, then a line in the shape of the other diagnostics that names the phase and the input it ran out in, such as `failed to rewrap file: out of memory ('<path>')`, and exits with `EXIT_CODE_FAILURE`, so no call site checks for allocation failure, and the domain cannot fail. Under the address sanitizer two arena allocations are never adjacent, so code that extends the last allocation in place uses `arena_extend`.
- **Text representation**: Every first-party string is a counted `struct Str` (`vendor/sharedstuff/include/shared/str.h`) with a signed `ptrdiff_t` length, owned or borrowed alike, and a `NUL` byte is an ordinary byte to it. A C string exists only where text reaches libc, POSIX or a vendored API that takes one: the POSIX host copies each path into a `PATH_MAX` buffer at the system call (`copy_cstr`), and each command-line argument is wrapped as a view where it is parsed or passed to `wrap_input`. Lengths, counts and columns are `ptrdiff_t`, and convert to `size_t` only at those calls. The lexer and wrapping pipeline use views into the source, and a refilled line is new text in the scratch.
- **Text input checks**: `os_read` and `os_read_stdin` reject input that contains a `NUL`. No input format gives a `NUL` a meaning, so these keep it out of the rewritten output. The check is a product decision, not something the code needs: allowing a `NUL` would delete the `memchr` checks in both implementations of each and the contract tests that pin them, `test_read_rejects_embedded_nul` and `test_read_stdin_rejects_embedded_nul_and_input_over_limit`. `os_read` reports this policy failure separately from system errors. A path holding a `NUL` fails at the `os` boundary with `EINVAL`, because a system call would read it only up to that byte.
- **Return values**: An action, and a producer that can fail, returns `0` or `-1`, and the producer writes its result through an `_out` parameter. A producer that cannot fail returns its result directly, such as a `struct Str` by value. A predicate returns `bool`. An allocation that does not fit is never a return value. An operation with a third, non-error outcome returns an enum; `wrap_input` uses `WrapInputResult` to keep a check-mode change distinct from an operational error.
- **Diagnostic logs**: A fallible action whose caller needs a message beyond success or failure takes a final `struct Log* log` and records every error it finds there, never truncated. A message is one `printf` format, and the `format` attribute makes the compiler check its arguments. A `struct Str` prints through `%.*s` and `STR_FMT`. Filesystem and stream actions also take the caller's `operation`, such as `failed to read file`, and log the whole message, because only they know the cause. A log never grows in an arena that a callee receives by value while the log is in use, so the log never grows into bytes a callee is using.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. A value follows the cause.
- **Input size limits**: Each `os_read` caller passes a limit for the kind of file it reads. The read checks the limit before it allocates, so an oversize file is never loaded. `SOURCE_LEN_MAX` in `wrap_input` is 64 MiB for each source file and for standard input. `os_read_stdin` has no size to check before it reads, so it stops one byte past the limit and rejects the input.
- **Host access**: Nothing in `src/` outside `runtime/fs.c` makes a system call on a path or a standard stream, or prints through `stdio`. Each executable chooses its host when it is linked. `cwrap` links `runtime/fs.c`, and so does every unit test registered without `OS memory`: the POSIX layer's own tests (`fs`, `os_posix`) and the test-support test, which need a real filesystem, the suites not yet converted to the in-memory host (`cli`, `cli_dispatch`), and the tests that reach no host. The tests registered with `OS memory` link [tests/os_memory.c](tests/os_memory.c), a set of files by path with a standard input, injected failures and captured output. A new test links it too, unless it needs a fact only a real filesystem has. `cwrap` takes paths and never lists a directory, so that host has no directory tree, symlinks or path resolution, which the POSIX-only tests in `test_fs.c` cover instead. A link-time choice needs no table of function pointers, because no executable needs two hosts. `src/runtime/test_os.c` builds once against each host and runs the same cases, so every property a test relies on in the in-memory host also holds on POSIX.
- **Line endings**: Each comment block records whether its source uses LF or CRLF. Refilled lines use that same convention, while untouched source bytes pass through unchanged.

## Design decisions

This section records each design decision and the reason for it. Read it before you change one of these designs. A change must answer the reason that this section gives.

These terms have one meaning in this section:

- **Block**: the one reservation a command makes when it starts. The command splits it into its arenas and releases it at its end.
- **Region**: a range of address space that one arena uses. Two arenas never use the same region at the same time.
- **Reserve**: to get address space from the system without memory behind it.
- **Commit**: to make reserved pages usable. The system charges committed pages against its memory limit.
- **Scratch**: the run's arena for temporary data. A function gets it by value, as `scratch`.
- **Input**: the run's arena for one input's source and rewritten output. A function gets a copy by value, as `input`, so the next input reuses it.
- **Perm**: an arena for data that must stay after the function returns. A function gets it by pointer, as `perm`, as `wrap_input_read_file` gets the copy of the input arena it reads a source into.
- **Log**: a `struct Log`, the error lines a command prints once. It grows in an arena of its own, which no callee gets by value.

### Memory: arenas only

**Decision.** All first-party memory comes from arenas. An arena is two pointers, `beg` and `end`, into one region, and a third, `limit`, when the region is reserved and commits pages on demand. The code never frees one allocation. A scratch goes by value, so its memory is free again when the function returns. A perm goes by pointer.

**Reasons.**

- No comment must say who frees a buffer.
- The bounds check is one comparison that cannot overflow.
- A `Buf` can grow in place when it is the last allocation. The rewritten output grows after its source in the input arena, and standard input grows in place while it is read.
- An arena is one region, not a list of chunks, so a callee that allocates in a by-value copy leaks nothing.

### Memory: a log, an input arena and a scratch, and no pool

**Decision.** `cwrap` runs on one thread and processes one input at a time, in three arenas:

- **The log.** It holds the run's errors, at most a diagnostic or two for each input, each quoting a path from the command line. `ARG_MAX` bounds the inputs to about 600,000 on Linux, so the log takes under 40 MB of its 64 MiB even when every path is one byte. Its size follows the number of inputs, not their size, so it keeps its full size under an address-space limit.
- **The input arena.** It holds one source and its rewritten output.
- **The scratch.** It holds the temporaries of one input. `rewrite_source` keeps the comment spans and blocks there, and gives each block its own copy by value.

`cmd_wrap_run` reserves the log, and then the input arena and the scratch in one block, when it starts. Each input gets the input arena and the scratch by value, so the next input uses the same memory again. `cli_dispatch` formats the command line's diagnostic and the help text in a 16 MiB reservation of its own, several times the largest `ARG_MAX` in use, and releases it once the run or the print returns.

**Reasons.**

- The run needs memory for its largest input, not for the sum of its inputs.
- The lines and atoms of one block are free again before the next block.
- The log grows while each input copies the input arena and the scratch by value, so the log needs an arena that no input gets.
- A diagnostic quotes an argument whole, and only the system's argument-size limit bounds an argument, so the command line's diagnostic goes in a reservation several times that limit.
- The command line's reservation has a fixed size rather than one from `sysconf(_SC_ARG_MAX)`, because glibc derives that value from `RLIMIT_STACK`, so it is very large when the stack is unlimited, and musl gives a different value.
- `cwrap` runs on one thread, because nothing argues for rewrapping inputs in parallel, and because `os_write` reads the umask by setting it and restoring it, and a diagnostic calls `strerror`, both of which are safe only with one thread.

### Memory: fixed sizes, reserved, then committed on demand

**Decision.**

1. The run reserves its memory with `PROT_NONE` when it starts, and releases it at its end: the log (`CMD_WRAP_LOG_LEN`, 64 MiB) on its own, and one block for the input arena (`CMD_WRAP_INPUT_LEN`, 8 GiB) and the scratch (`CMD_WRAP_SCRATCH_LEN`, 8 GiB). The reservations cost no memory.
2. An arena commits more pages only when an allocation does not fit in the committed part. The fast path stays one bounds check. The slow path commits pages a mebibyte past what the allocation needs, up to the end of the region.
3. The sizes are fixed, from the input limit and the shapes measured, not predicted from the inputs. The output and the temporaries grow linearly with the source for every shape but two, and the worst linear shape measured took under 90 bytes of scratch for each source byte, under 6 GiB at `SOURCE_LEN_MAX`. A comment indented by about half its line writes an output that grows with the square of the source, 2.45 GB for a 140 KB file at `-w 20`. A Doxygen `@param` with a long name at a narrow width grows both: a 60,033-byte file at `-w 1` writes 400 MB and takes about 6,685 bytes of scratch for each source byte. A buffer at most doubles past what it holds, so the input arena holds an output of up to 4 GiB.
4. If the system refuses the block, as under a `ulimit -v` below it, `arena_reserve_halving` halves it, and both arenas with it, up to six times, and halves the first size that fits once more, so the rest of the process keeps room. If even the smallest size is refused, the run ends with the out-of-memory report. A run under a limit of a few GiB still handles ordinary inputs, and an input that outgrows the smaller arenas runs out of memory, with the report below.

**Reasons.**

- A fixed size needs no pass over the inputs before the first read, no budget, and no refusal messages.
- Reserving costs system time in proportion to the size, about 1 µs per GiB to reserve and as much to release, on macOS, so the arenas are sized from their bounds rather than as large as the system allows: 65 GiB costs a tiny run about 0.2 ms, 7% of its time.
- An arena commits its pages on demand, not all at once, because strict overcommit charges every committed page against the system's limit, and Linux ignores `MAP_NORESERVE` under it.

**Cost.** Under a `ulimit -v` below the reserved total, the arenas are smaller, and an input that outgrows them runs out of memory, where a `malloc` design might have fitted it. An output over 4 GiB runs out of memory with no limit set, and the two shapes that grow with the square of the source, which `cmd_wrap.c` describes, reach that size, or the scratch's, from a few hundred kilobytes of source at a narrow width.

### In-place mode checks every input before the first write

**Decision.** In in-place mode a pass without writes first reads every path with `wrap_input_read_file`, and logs each one that is missing, cannot be read, is not a regular file, is larger than `SOURCE_LEN_MAX`, or holds a `NUL`, in path order, in `os_read`'s own words. If the log has an error after that pass, the run writes no file. The default mode reads each file once, in order, and stops at the first bad path, after printing the files before it. Check mode writes nothing, reads each file once, and reports every input in path order.

**Reasons.**

- A bad input among many leaves every file as it was, and one run reports every bad input. Printed output can be run again, so the default mode keeps one read of each file.
- The pass costs a second open and read of every file in in-place mode, which one `fsync` for each written file hides.
- A run that has an error writes nothing, rather than rewriting the good inputs as `clang-format -i` does, so the user fixes the bad input and runs the same command again.

**Limit.** A write that fails, or a file that changes during the run, still stops the run part-way, after the inputs before it were written. Each write is atomic, so no file is left half-written. A file given twice is written once, at its first position, and found already wrapped at its second.

### Out of memory exits the process

**Decision.** When an arena cannot commit more pages, it calls `os_oom`. Each program defines `os_oom`. The `cwrap` version, in `main.c`, calls `memory_oom_report`, which does these steps:

1. It prints the errors that the log of the command has. `cmd_wrap_run` names its log with `memory_oom_set_log`.
2. It prints a line in the shape of the other diagnostics, with the phase and the input: `failed to rewrap file: out of memory ('<path>')`, `failed to read input: out of memory (from 'stdin')`, or `failed to run: out of memory` outside an input. An input names its phase and itself with `memory_oom_set_job` before each phase.

Then `os_oom` exits with `EXIT_CODE_FAILURE`.

**Reasons.**

- `cwrap` is a CLI tool, and its in-place write is atomic, so each file that a stopped run leaves is either as it was or completely rewritten.
- No call site checks for allocation failure, and the domain cannot fail.
- The report names the input the run was working on, which a user needs to find the input that failed. It has no program-name prefix, because no other message of `cwrap` has one.

**Vendored code.** No vendored library in the program allocates. `copt` parses the arguments in place.

### Address sanitizer coverage in arenas

**Decision.** In a build with the address sanitizer, an arena over a reserved region poisons the pages it commits. `arena_alloc` unpoisons only the bytes that it returns. In such an arena it leaves a poisoned gap before each allocation, so two of its allocations are never adjacent. An arena over memory the caller owns has no gap.

**Reasons.** ASan cannot see an overflow that stays inside one large mapping. Without this step, the debug preset loses most of its overflow checks.

**Limit.** The arena does not poison scratch memory again when a function returns. ASan does not find a use of scratch memory after the function that owned it returns.

### A log must not grow in a borrowed arena

**Decision.** A `struct Log` and its `Buf` never grow in an arena that a callee receives by value while the log is in use. The run gives its log an arena of its own.

**Reason.** A `Buf` grows in place. Assume that a callee gets a copy of the same arena by value. The callee then allocates the same bytes that the log grows into. The two overwrite each other with no crash and no ASan report.

**How to apply.** [STYLE.md](STYLE.md#memory-and-resources) states the rule. Treat a break of it as a bug, not as a style problem.

### Counted strings with signed lengths

**Decision.** Every first-party string is a `struct Str`, a pointer and a `ptrdiff_t` length. Every length, count and column is `ptrdiff_t`. A value changes to `size_t` only at a libc, POSIX, or vendored call, or in a C buffer that goes to one, such as the `PATH_MAX` buffer of `copy_cstr`. Each such change asserts that the value is not negative, unless that holds by construction. A C string exists only where libc, POSIX, or a vendored API needs one.

**Reasons.**

- Subtraction cannot wrap to a very large value.
- A `NUL` byte is an ordinary byte, so no code calls `strlen` on owned text, and a body line is a view into the source until a refill builds new text.

**Kept on purpose.** `os_read` and `os_read_stdin` reject input that contains a `NUL`. This is a product decision, and the string type does not force it. If the product decision changes, the `memchr` checks in both hosts and the two contract tests that pin them can go.

### A growable log, and every error in one run

**Decision.**

- Each fallible function whose caller needs a message takes `struct Log* log` as its last parameter. The log keeps every error with no truncation.
- Check mode and in-place mode report every bad input in one run, and the default mode stops at the first, as the in-place decision above describes.
- A rejected command line reports its first error only, because the later errors are usually results of the first.

**Reasons.**

- A user sees all errors in one run.
- No message buffer has a fixed size, so no message is truncated, and none is ordered to survive truncation.

### Log messages use checked format strings

**Decision.** A log call takes one `printf` format and its arguments. `log_error`, `log_error_in`, `log_error_v`, `buf_printf`, and `buf_vprintf` have `__attribute__((format(printf, ...)))`. A `struct Str` prints with `%.*s` and `STR_FMT`. An integer prints with the conversion of its type. An example:

```c
log_error(log, "failed to read file: %s ('%.*s')", reason, STR_FMT(path));
```

**Reasons.**

- Each message is one sentence in the source. A reader can read the wording as one line, and `grep` finds it.
- The compiler checks the number and the types of the arguments against the format. `-Wformat=2` is on for first-party code and for `sharedstuff`.
- `%td` prints a `ptrdiff_t` with no cast. Only a string needs a helper, `STR_FMT`.
- A first-party function that formats a message gets the same check: it has the attribute.

**Costs and limits.**

- `STR_FMT` evaluates its argument two times. Give it a variable or another lvalue. Put a computed string in a variable first.
- A string longer than `INT_MAX` bytes trips an assertion at run time, because the `%.*s` precision is an `int`.
- The attribute is the only guard on the code that reads the arguments. Each formatting function measures the text with `vsnprintf`, allocates it, and writes it with a second `vsnprintf`: `buf_printf` and `buf_vprintf` in `sharedstuff`, which grow the buffer in place, and the first-party functions that format their own message. The measured `int` length becomes a `ptrdiff_t` only in those functions.
- A first-party variadic function does not pass its `va_list` on to `log_error_v` or `buf_vprintf`. Clang's analyzer, version 22, targeting AArch64 Linux, where a `va_list` is a structure, reports a false `va_end` on an uninitialized list when it does. The function starts its list once for each `vsnprintf` pass instead, as `buf_printf` does: `record_error` in `cli.c`, and `test_log_is` and `test_text_is` in `tests/test_support.c`.
- `%.*s` stops at a `NUL` byte. The input checks reject a `NUL` in file content, on standard input, and in paths at the `os` boundary, and a message quotes only the command-line arguments and the name `stdin`, so a message gets no `NUL` from input. A path that holds a `NUL` is quoted only up to that byte in its `EINVAL` message. The contract test for this shows the result.
- An `off_t` has no conversion of its own. It prints with `%jd` and a cast to `intmax_t`.
- `clang-format` breaks a string literal that passes the column limit. Some long messages are in two literals.

### The `os` interface

**Decision.** All host access goes through `src/runtime/os.h`: `os_read`, `os_read_stdin`, `os_write`, `os_print`, and `os_oom`. `fs.c` is the POSIX implementation. `tests/os_memory.c` is an in-memory implementation for unit tests. The link step selects one implementation for each executable. A function that touches the host takes `struct Os*`, just before its arenas, and an `os_*` function takes it first.

**Reasons.**

- The tests of the command, of each input and of the out-of-memory report need no temporary files and no redirected descriptors. The in-memory host also injects the failures that are hard to cause on POSIX, such as a failed write to `stdout` or a failed in-place write.
- `os_print` carries every byte that the program prints, `--help` and `--version` included, so a test sees all of it, and a failed write is reported where it occurs, with its input.
- Host access is visible at one boundary, not spread over calls to `stdio` in the app layer.

**The contract is `cwrap`'s own.** `os_read` opens a path rather than calling `stat`, because `stat` does not see that the process may not read a file. `os_write` rewrites the source of a user, so it is atomic and synced: it writes, `fsync`s and closes a temporary file next to the destination, then renames it over the destination. The in-memory host has no directory tree, symlinks or path resolution, because `cwrap` takes paths and never lists a directory. The POSIX-only tests in `test_fs.c` cover those.

**Condition.** Each property that a test checks against the in-memory host must have a contract test. One test source compiles into two executables: one links the POSIX host and one links the in-memory host. Both must pass. Without the contract tests, the in-memory host can accept what POSIX rejects. For example, an in-memory host could accept `file/` as a file, but POSIX returns `ENOTDIR`, and a contract test checks it.

### Seeded random tests in the normal suite

**Decision.** `tests/test_random.c` checks two properties on generated files: a second rewrap changes nothing, and the bytes outside comments do not change, apart from the line breaks that a refill moves between the lines of one `//` run. It draws the files from the generator in `tests/test_support`, a 64-bit linear congruential step, from a fixed seed (`TEST_RNG_SEED_DEFAULT`), so every run checks the same cases. A failing check names the case.

**Reasons.**

- The idempotence suite checks the fixed-point invariant only on fixed files. The pass checks it on files that nobody wrote by hand.
- The pass runs in every test preset, under its sanitizers, with no fuzzing engine.
- A fixed seed keeps the suite deterministic, so a failure appears in every run.

**Limit.** The pass reaches only the input shapes its generator lists. Searching beyond them is the work of a fuzzing harness, which the tree does not have yet.

### Where the shared code lives

**Decision.** `sharedstuff` holds the generic parts: the arena with reserve, commit, and poisoning, `Buf`, `Str`, and `Log`. Each program defines `os_oom`. Each program keeps its own `os` interface and its own arena layout. `arena_release` does not unpoison a whole region under the address sanitizer, which would write an eighth of each reservation as resident shadow.

**Reason.** These parts do not depend on what a program does.
