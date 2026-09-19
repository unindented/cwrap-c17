# Architecture

`cwrap` is a small C17 CLI that rewraps C comments. It reads source files, refills wrapable comment blocks, and writes the result to `stdout`, in place, or as a `--check` report. This document describes the code structure. See [README.md](README.md) for usage and wrapping behavior.

## Layout

- [src/app/](src/app/): Process entry point, command-line interface, and command boundary.
- [src/domain/](src/domain/): Comment lexing, grouping, filling, Doxygen alignment, and source rewriting.
- [src/runtime/](src/runtime/): Filesystem services that interact with the host.
- [src/core/](src/core/): Small reusable primitives for diagnostics, parsing, and ASCII classification.
- [tests/](tests/): Golden and idempotence test suites. Each C file under [tests/fixtures/](tests/fixtures/) has a matching expected output under [tests/expected/](tests/expected/). The idempotence check also rewraps these files and every first-party source file at several widths.
- [CMakeLists.txt](CMakeLists.txt) and [CMakePresets.json](CMakePresets.json): Project entry point and supported build configurations.
- [cmake/](cmake/): Build profiles, quality tools, packaging, the golden and idempotence test drivers, and reusable cross toolchains. [BUILD.md](BUILD.md) documents their boundaries and policy.
- [vendor/](vendor/): Bundled dependencies (`copt`, `acutest`).

First-party dependencies point inward:

- `app` can depend on `domain`, `runtime`, and `core`.
- `domain` and `runtime` can depend on `core`.
- `core` cannot depend on another first-party directory.

A module can skip layers. Modules in the same directory can depend on each other. First-party include directives start at the `src/` root. For example, a module includes `"core/error.h"`. This convention makes each dependency visible at the call site. Vendored support is outside the first-party hierarchy.

## Entry point and command

`main` ([src/app/main.c](src/app/main.c)) only calls `cli_dispatch` ([src/app/cli_dispatch.h](src/app/cli_dispatch.h)). This separation lets unit tests link the code that maps an action to an exit code. `cli_dispatch` uses `cli_parse` ([src/app/cli.h](src/app/cli.h)) to parse the command line. `cli_parse` selects one of four actions: version, help, run, or error. For the run action, `cli_dispatch` calls `cmd_wrap_run` ([src/app/cmd_wrap.h](src/app/cmd_wrap.h)).

`cli_parse` is the one translation unit that defines `COPT_IMPL`. It never prints or exits. Every command returns `enum ExitCode` ([src/app/exit_code.h](src/app/exit_code.h)): `EXIT_CODE_OK` (0), `EXIT_CODE_FAILURE` (1), or `EXIT_CODE_USAGE` (2).

The command has two layers:

- `cmd_wrap_run` is the command boundary. It iterates through the input paths and delegates each file to `wrap_file_process` ([src/domain/wrap_file.h](src/domain/wrap_file.h)). Check mode continues after both changed and error results so it can report every input that would change and every processing failure. The boundary flushes default-mode output and prints any collected diagnostic to `stderr` exactly once.
- `wrap_file_process` owns the resources for one input and runs its read, rewrite, and emit steps. It returns a `WrapFileResult` that distinguishes success, a check-mode change, and an operational error. It writes rewritten source to `stdout` in the default mode, writes only a changed source in in-place mode, and writes nothing in check mode.

No domain or runtime module writes a failure diagnostic directly to `stderr`. Each fallible operation returns errors to its caller through a fixed `(char* err, size_t err_len)` or `(char* reason, size_t reason_len)` pair. The command appends per-file failures to a growable `StringBuffer`.

## Wrap pipeline

`rewrite_source` ([src/domain/rewrite.h](src/domain/rewrite.h)) owns the domain pipeline for one source buffer:

1. **Lex** (`lex_comment_spans`): A character state machine records every comment span and ignores strings and character literals. A shared transition keeps LF and CRLF line splices inside the state they continue.
2. **Group** (`comment_group`): Consecutive same-indent `//` lines become one block. A run is grouped without regard to a backslash line splice, so two `//` lines joined by one collapse into a single line whose `\` becomes literal text. The result is still wholly a comment, and a trailing `\` that ends a payload stays the last atom, so refilling never moves a splice off the end of a line and turns the spliced line into code. Trailing comments are omitted. A slash-star block is starred when a continuation `*` sits at the decoration column (`opener + 1`). A `*` under the hanging prose is a list marker.
3. **Extract** (`comment_extract_body`): Decoration is stripped (`*/` before `*`) and each payload line is classified. A closer-only last line is dropped. Hanging indent is stripped; extra indent that is not a tag continuation is an indented sample.
4. **Align** (`doxygen_align_parameters`): `@param` descriptions share a column.
5. **Fill** (`fill_body_lines`): Prose paragraphs are greedily filled with [STYLE.md](STYLE.md) atoms.
6. **Emit** (`comment_emit`): Prefixes and closers are restored with the block's original LF or CRLF convention.
7. **Splice**: Non-comment bytes are copied unchanged.

## Modules

### Domain (`src/domain`)

- [lex](src/domain/lex.h): Finds comments without treating string literals, character literals, or include paths as comments. It records source spans, line positions, indentation, and whether a comment trails code.
- [comment](src/domain/comment.h): Groups comment spans into blocks, extracts classified body lines, and reconstructs the selected comment shape.
- [atom](src/domain/atom.h): Measures Unicode 16.0 display-column width and splits prose into unbreakable wrapping atoms.
- [fill](src/domain/fill.h): Greedily fills prose paragraphs while preserving samples, lists, tags, and hanging indentation.
- [doxygen](src/domain/doxygen.h): Aligns `@param` descriptions and their continuation lines.
- [rewrite](src/domain/rewrite.h): Runs the domain pipeline over one source buffer and splices rewritten comments between unchanged source ranges.
- [wrap_file](src/domain/wrap_file.h): Reads, rewrites, and emits one source according to the selected output mode.

### Runtime (`src/runtime`)

- [fs](src/runtime/fs.h): Reads stable, regular, `NUL`-free files and atomically writes byte buffers. Reads return a terminated allocation plus its byte length and reject files that change size during the operation. Writes close a sibling temporary file before renaming it over the destination, preserving an existing file on failure.

### Core (`src/core`)

- [ascii](src/core/ascii.h): Locale-independent ASCII byte classification.
- [error](src/core/error.h): Uniform diagnostic reporting for fallible actions.
- [parse](src/core/parse.h): Parsing of terminated text into scalar values.

## Fixed-point invariant

Rewrapping is idempotent: a second pass over the output of a first must change nothing, and `--check` must agree. This is what lets `--check` serve as a CI gate, since a shape `comment_emit` cannot reproduce from its own output would report a rewrap for a file `--in-place` had just written. The constraint falls on the emitter: every closer, prefix, and indent it writes has to re-parse to the same block shape. A starred block whose refilled prose fits on the opener line therefore trails its closer rather than orphaning it on a line of its own, because a lone closer would re-parse as a line to join.

## Cross-cutting conventions

- **Ownership**: Arenas own the temporary allocations made while rewriting one source. File reads return a separate buffer that the caller frees. A `StringBuffer` owns its growable allocation until it is freed or `string_buffer_steal` transfers that allocation to the caller.
- **Text representation**: A file read produces a terminated, `NUL`-free buffer plus its byte length. The lexer and wrapping pipeline use borrowed slices into that buffer. The embedded-`NUL` check prevents silent truncation when the buffer is used as a C string.
- **Line endings**: Each comment block records whether its source uses LF or CRLF. Refilled lines use that same convention, while untouched source bytes pass through unchanged.
- **Return values**: A producer returns a pointer or `NULL`. An action returns `0` or `-1`. A predicate returns `bool`. An operation with a third, non-error outcome returns an enum; `wrap_file_process` uses `WrapFileResult` to keep a check-mode change distinct from an operational error.
- **Diagnostic buffers**: A fallible action can take a final `(char* err, size_t err_len)` pair. `error_report` fills this buffer and marks truncated text with `...`. Filesystem actions use a `(char* reason, size_t reason_len)` pair instead. This pair contains a reason fragment. The caller adds the operation and file information.
- **Diagnostic text**: First-party diagnostics use lowercase prose and name the failed operation first. They use single quotes for literal names and values. External messages keep their original capitalization and punctuation. An unbounded value follows the cause, so truncation removes the value instead of the reason.
