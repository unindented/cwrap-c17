<div align="center">
  <img src="media/logo.webp" height="300" alt="">
</div>

<h1 align="center"><code>cwrap</code> /kræp/</h1>

This is a small C comment formatter written in C17.

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
> I used LLMs extensively to create this project, mostly Claude Opus 4.8 and 5.

## Installation

Download a prebuilt binary from the [Releases](https://github.com/unindented/cwrap-c17/releases) page, or [build from source](#contributing).

## Usage

`cwrap` accepts one or more input files:

- `cwrap [options] <file>...`: Rewrap comments in each input file.

These options control the wrapping and output mode:

- `-w, --width N`: Wrap at column `N`. The value must be from 1 through 10000. The default is 100.
- `-i, --in-place`: Rewrite changed input files in place.
- `-c, --check`: Write no files and exit non-zero if any input would change.

(Options `--check` and `--in-place` are mutually exclusive.)

Run these commands for help:

- `cwrap --version` (or `-V`): Print the version.
- `cwrap --help` (or `-h`): Print usage and the available options.

## Contributing

### Prerequisites

- POSIX-like system with CMake 3.25 or later and a C17 compiler such as Clang or GCC.
- Optional build tools: `ninja` for the multi-config portability check.
- Optional quality tools: `clang-format`, `clang-tidy`, `cppcheck`.

Vendored dependencies are included under `vendor/`:

- [`copt`](https://github.com/fardaniqbal/copt): Command line option parsing.
- [`acutest`](https://github.com/mity/acutest): Tests.

### Building

Presets keep every build out of the source tree. The commands below use eight parallel build jobs. Adjust that number for the machine.

#### Debug build

The debug build enables `AddressSanitizer` and `UndefinedBehaviorSanitizer`. It also runs `clang-tidy` and `cppcheck` during compilation when they are available.

```sh
cmake --preset debug
cmake --build --preset debug -j 8
```

The binary is `build/debug/bin/cwrap`.

To run linting after configuring this preset:

```sh
cmake --build --preset lint
```

#### Release build

The release preset uses `RelWithDebInfo`, treats compiler warnings as errors, and does not build the tests. The build-tree binary retains its debug information. Packaging strips the installed copy.

```sh
cmake --preset release
cmake --build --preset release -j 8
```

The binary is `build/release/bin/cwrap`.

#### Multi-config build

The multi-config preset uses the `Ninja Multi-Config` generator. One configured tree can build both configurations:

```sh
cmake --preset multi
cmake --build --preset multi-debug -j 8
cmake --build --preset multi-relwithdebinfo -j 8
```

The binaries are `build/multi/bin/Debug/cwrap` and `build/multi/bin/RelWithDebInfo/cwrap`.

### Testing

CTest registers the colocated unit test suite and the end-to-end golden and idempotence test suites.

#### Debug tests

```sh
cmake --preset debug
cmake --build --preset debug -j 8
ctest --preset debug -j 8
```

To select part of the debug suite:

```sh
ctest --preset debug -L unit -j 8
ctest --preset debug -L golden -j 8
ctest --preset debug -L idempotent -j 8
ctest --preset debug -R rewrite -j 8
```

#### Multi-config tests

Each configuration must be named when building and testing:

```sh
cmake --preset multi

cmake --build --preset multi-debug -j 8
ctest --preset multi-debug -j 8

cmake --build --preset multi-relwithdebinfo -j 8
ctest --preset multi-relwithdebinfo -j 8
```

The `multi-relwithdebinfo` test preset exercises the same CMake configuration used by the release build. The release preset itself does not build tests.

#### CI workflows

Workflow presets run the complete configure, build, and test sequences used by CI:

- `cmake --workflow --preset ci-debug`: `Debug` build, linting, and all ASan/UBSan tests.
- `cmake --workflow --preset ci-release`: `Release` build.
- `cmake --workflow --preset ci-multi`: `Debug` and `RelWithDebInfo` builds and tests under the `Ninja Multi-Config` generator.

To run the same Linux workflows from a machine with Podman, build the pinned Ubuntu image:

```sh
podman build --tag cwrap-linux-ci --file Containerfile .
```

The image contains a snapshot of the source tree, so container builds do not mix Linux products with the host's `build/` directory. Run each workflow preset in a fresh container:

```sh
podman run --rm cwrap-linux-ci ci-debug
podman run --rm cwrap-linux-ci ci-tsan
podman run --rm cwrap-linux-ci ci-release
podman run --rm cwrap-linux-ci ci-multi
```

The image supports x86-64 and AArch64 hosts and pins LLVM 22. Zig remains a release-only dependency and is not included in the CI image.

See [BUILD.md](BUILD.md) for the target graph and the reasons behind these configurations.

### Packaging

Only release configurations have package presets. There is intentionally no debug package.

#### Native release package

```sh
cmake --preset release
cmake --build --preset release -j 8
cpack --preset release
```

The stripped archive is written to `build/release/`.

#### x86-64 Linux `musl` package

This cross-build requires Zig 0.16 and `llvm-strip`.

```sh
cmake --preset release-linux-x86_64
cmake --build --preset release-linux-x86_64 -j 8
cpack --preset release-linux-x86_64
```

The archive is `build/release-linux-x86_64/cwrap-<version>-x86_64-linux-musl.tar.gz`.

#### AArch64 Linux `musl` package

This cross-build also requires Zig 0.16 and `llvm-strip`.

```sh
cmake --preset release-linux-aarch64
cmake --build --preset release-linux-aarch64 -j 8
cpack --preset release-linux-aarch64
```

The archive is `build/release-linux-aarch64/cwrap-<version>-aarch64-linux-musl.tar.gz`.

#### Source package

A source package needs configuration but does not need a compiled binary:

```sh
cmake --preset release
cpack --config build/release/CPackSourceConfig.cmake
```

The archive is `build/release/cwrap-<version>-source.tar.gz`. It excludes build products and private workspace metadata.

Every archive version comes from `project(VERSION)` in `CMakeLists.txt`. A release commit must be tagged with the matching `vMAJOR.MINOR.PATCH`.
