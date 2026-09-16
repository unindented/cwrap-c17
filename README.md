# cwrap /kræp/

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

## Prerequisites

- POSIX-like system with `make` and a C17 compiler such as Clang or GCC.
- Optional quality tools: `clang-format`, `clang-tidy`, `cppcheck`.

Vendored dependencies are included under `vendor/`:

- [`copt`](https://github.com/fardaniqbal/copt): Command line option parsing.
- [`acutest`](https://github.com/mity/acutest): Tests.

## Building

- `make` or `make debug`: Build with ASan/UBSan and fatal warnings.
- `make release`: Build an optimized binary with fatal warnings.
- `make tsan`: Build a TSan binary as `cwrap-tsan`.
- `make test-debug`: Build and run debug unit tests.
- `make golden-debug`: Rewrap every fixture and diff its debug golden output.
- `make idempotent-debug`: Check fixtures, expected outputs, and source files for fixed-point wrapping at several widths.
- `make format`: Format `src/` and `tests/`.
- `make lint`: Run linters.
- `make ci`: Run linters, the release build, unit tests, the golden diff, and the idempotence check.
- `make clean`: Remove build outputs.

## Running

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
