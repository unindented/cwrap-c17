<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>cwrap</code> /kræp/</h1>

`cwrap` is a small C comment formatter written in C17. It refills the prose in C comments to a column width.

For example, running `cwrap -w 72` converts this:

```c
/**
 * @brief Copies `n` bytes from `src` to `dest`. The two regions must not overlap.
 * @param dest Destination buffer. It must be large enough for `n` bytes.
 * @param src Source buffer. It is read for `n` bytes.
 * @param n Number of bytes to copy.
 * @return `dest`.
 */
void* memcpy(void* dest, const void* src, size_t n) {
  unsigned char* d = dest;
  const unsigned char* s = src;

  // Walk both pointers forward until every requested byte has been copied into `dest`.
  while (n--)
    *d++ = *s++;
  return dest;
}
```

into this:

```c
/**
 * @brief Copies `n` bytes from `src` to `dest`. The two regions must
 *        not overlap.
 * @param dest Destination buffer. It must be large enough for `n`
 *             bytes.
 * @param src  Source buffer. It is read for `n` bytes.
 * @param n    Number of bytes to copy.
 * @return `dest`.
 */
void* memcpy(void* dest, const void* src, size_t n) {
  unsigned char* d = dest;
  const unsigned char* s = src;

  // Walk both pointers forward until every requested byte has been
  // copied into `dest`.
  while (n--)
    *d++ = *s++;
  return dest;
}
```

## LLM disclosure

> [!warning]
> I used LLMs extensively to create this project, mostly Claude Opus 4.8, 5, and 5.5.

## Installation

Download a prebuilt binary from the [Releases](https://github.com/unindented/cwrap-c17/releases) page, or [build it from source](#building).

## Usage

### Inputs and options

`cwrap` reads files or standard input:

- `cwrap [options] <file>...`: Rewrap the comments in each input file.
- `cat foo.c | cwrap [options]`: Read standard input when the command line names no file.
- `cwrap [options] - < foo.c`: Read standard input. The `-` argument selects standard input explicitly.

These options control the wrapping and the output mode:

- `-w, --width N`: Wrap at column `N`. `N` must be from 1 through 10000. The default is 100.
- `-i, --in-place`: Write the result over each input file that the rewrap changes.
- `-c, --check`: Do not write any file. Exit with status 1 if the rewrap would change any input.

There's also these options:

- `cwrap --version` (or `-V`): Print the version.
- `cwrap --help` (or `-h`): Print the usage and the list of options.

The tool rejects these combinations of inputs and options:

- Standard input with file inputs.
- Standard input with `--in-place`.
- `--check` with `--in-place`.

`cwrap` exits with status 0 when it succeeds, 1 when it fails or when `--check` finds an input that would change, and 2 when it rejects the command line.

### Failures

An input file is bad when it:

- does not exist
- is not a regular file
- cannot be read
- contains a `NUL` byte

In the default mode, the tool prints the result of each file in turn. At the first bad input file, the tool stops and exits with status 1. It prints the results of the files before the bad file.

With `--in-place`, the tool first reads every input file and reports each bad file. If any input file is bad, the tool writes no file. A write can still fail after this first pass. Then the tool stops at that file, and the files before it keep their new contents.

`--check` never writes a file, and it reports every input in order.

### Limits

The tool rejects an input file larger than 64 MiB before it reads the file. The error message gives the limit and the file size. The tool rejects standard input larger than 64 MiB when it reads one byte past the limit. This error message gives the limit.

The tool reserves its memory once, when it starts, in fixed sizes that come from these limits. Reserved memory that the tool does not use takes address space but no memory. An address-space limit (`ulimit -v`) can refuse the full reservation. When that happens, the tool makes its reservation smaller until it fits, so the tool has less room for its data. The log of the errors keeps its full size of 64 MiB in all cases. [ARCHITECTURE.md](ARCHITECTURE.md#memory-reserve-then-commit-on-demand) describes this process. These fixed sizes have two results:

- The tool needs about 360 MB (`ulimit -v 350000`). With a lower limit, the tool stops at startup with `failed to run: out of memory`.
- An input can need more memory than the tool reserved, for example a comment whose rewrapped output is larger than about 4 GiB. Then the tool prints the output and the errors up to that point, prints `failed to rewrap file: out of memory ('<path>')`, and exits with status 1.

## Wrapping behavior

- The tool refills the prose in `//`, `///`, `//!`, `/* */`, `/** */`, and `/*! */` comments to the column. Each comment keeps its opener. Consecutive line comments merge only when they have the same opener and the same indent, at the left margin or inside a function. A change of indent, a code line or a blank line separates two runs of line comments.
- The tool does not change a slash-star comment on one line when the comment fits the column and contains no tab, so a label such as `/* op    */`, with padding that aligns it with its neighbors, stays aligned.
- The tool does not change trailing comments, list items, indented samples, Markdown fences, or comments with no letters or digits, such as banners.
- A comment line that starts with a tool directive stays unchanged on its own line, even when the line is wider than the column. The prose before the directive and the prose after it are separate paragraphs. The directives are `cppcheck-suppress`, with any suffix, such as `cppcheck-suppress-file`; `NOLINT`, `NOLINTNEXTLINE`, `NOLINTBEGIN` and `NOLINTEND`, with or without a `(check)` suffix; `clang-format off` and `clang-format on`; and `IWYU pragma:`. Each directive matches only as a whole word, so `NOLINTED` is prose.
- In a Doxygen comment (`///`, `//!`, `/**`, or `/*!`), a line that starts with a command such as `@brief` or `\param` starts a new paragraph. The description of the command wraps under the first word after the command. The descriptions of all `@param` commands in a comment align in one column, and the descriptions of all `@retval` commands align in another column. The tool does not change `@code` and `@verbatim` regions.

## Development

[ARCHITECTURE.md](ARCHITECTURE.md) describes the code. [BUILD.md](BUILD.md) describes the CMake targets and the reasons for the configurations below. [STYLE.md](STYLE.md) describes the code style.

### Prerequisites

- A POSIX-like system with CMake 3.25 or later and a C17 compiler such as Clang or GCC.
- Optional build tools: `ninja` for the `multi` preset.
- Optional quality tools: `cwrap`, `clang-format`, `clang-tidy`, `cppcheck`.
- Zig 0.17 and `llvm-strip` for the Linux `musl` archives.

The source tree includes the dependencies in [vendor/](vendor/).

### Building

Each preset builds in `build/<preset>`, outside the source tree, and treats compiler warnings as errors. If `CC` is set, the preset builds in `build/<preset>/<CC>` instead, such as `build/debug/gcc-14`. CMake reads `CC` only when it configures a directory for the first time, so each compiler needs its own CMake cache. The commands use eight parallel jobs. Change that number to suit your machine.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

| Preset | Configuration | Binary |
| --- | --- | --- |
| `debug` | `Debug` with AddressSanitizer and UndefinedBehaviorSanitizer. It runs `clang-tidy` and `cppcheck` during compilation if they are available. | `build/debug/bin/cwrap` |
| `release` | `RelWithDebInfo`, with no tests. The binary in the build tree keeps its debug information, and packaging strips the installed copy. | `build/release/bin/cwrap` |
| `multi` | `Debug` and `RelWithDebInfo` in one build tree, with the `Ninja Multi-Config` generator. Build with `--preset multi-debug` or `--preset multi-relwithdebinfo`. | `build/multi/bin/Debug/cwrap`, `build/multi/bin/RelWithDebInfo/cwrap` |

`clang-tidy` runs only with Clang, because it rejects the warning options of GCC. To run linting, configure the `debug` preset, and then run this command:

```sh
cmake --build --preset lint
```

### Testing

CTest registers the unit tests and the end-to-end golden and idempotence tests. Build a preset, and then run its tests:

```sh
ctest --preset debug -j 8
```

The test presets are `debug`, `multi-debug`, and `multi-relwithdebinfo`. The `multi-relwithdebinfo` tests check the same CMake configuration as the `release` preset, which builds no tests. To run only part of a suite, select the tests by label or by name pattern:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -L idempotent -j 8
ctest --preset debug -R rewrite -j 8
```

### Continuous integration

The workflow presets run the same configure, build, and test steps as CI:

| Workflow | Steps |
| --- | --- |
| `cmake --workflow --preset ci-debug` | Build in `Debug`, run linting, and run all ASan and UBSan tests. |
| `cmake --workflow --preset ci-release` | Build in `RelWithDebInfo`. |
| `cmake --workflow --preset ci-multi` | Build and test in `Debug` and `RelWithDebInfo` with the `Ninja Multi-Config` generator. CI runs this workflow a second time with `CC=gcc-14` to check GCC. |

To run the Linux workflows on a machine with Podman, build the pinned Ubuntu image from [Containerfile](Containerfile). Then run each workflow in a new container:

```sh
podman build --tag cwrap-linux-ci --file Containerfile .
podman run --rm cwrap-linux-ci ci-debug
podman run --rm cwrap-linux-ci ci-release
podman run --rm cwrap-linux-ci ci-multi
podman run --rm -e CC=gcc-14 cwrap-linux-ci ci-multi
```

The image contains a snapshot of the source tree, including the vendored dependencies, without `.git/` and `build/`, which [.containerignore](.containerignore) excludes, so the Linux build products stay in the container and do not go into the `build/` directory of the host. The image supports x86-64 and AArch64 hosts, and pins LLVM 23 and GCC 14. It does not include Zig, because only the release archives need Zig.

### Packaging

Only the release configurations have package presets. Build a preset, and then package it:

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

| Preset | Archive |
| --- | --- |
| `release` | A stripped native archive in `build/release/`. |
| `release-linux-x86_64` | `build/release-linux-x86_64/cwrap-<version>-x86_64-linux-musl.tar.gz`, cross-built with Zig. |
| `release-linux-aarch64` | `build/release-linux-aarch64/cwrap-<version>-aarch64-linux-musl.tar.gz`, cross-built with Zig. |

A source archive needs a configured tree but no compiled binary. It does not contain build products or private workspace metadata:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The source archive is `build/release/cwrap-<version>-source.tar.gz`.

The version of every archive comes from `project(VERSION)` in [CMakeLists.txt](CMakeLists.txt). Tag each release commit with the matching `vMAJOR.MINOR.PATCH`.

## Dependencies

[vendor/](vendor/) contains these dependencies. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists their licenses.

- [sharedstuff](https://github.com/unindented/sharedstuff-c17): Arena allocation, counted strings, output buffers, and the diagnostic log.
- [copt](https://github.com/fardaniqbal/copt): Command-line option parsing.
- [acutest](https://github.com/mity/acutest): Unit tests.

## License

`cwrap` is MIT-licensed, as [LICENSE.txt](LICENSE.txt) states.
