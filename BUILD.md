# Build system

`cwrap` requires CMake 3.25 or later. This document explains the build targets and their relationships.

## Layout

The top-level CMake file controls the build. It sets project policy, loads build settings, and adds each source directory.

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Build the private application library and the `cwrap` executable |
| `src/*/CMakeLists.txt` | Add sources, header file sets, and local unit tests; `src/runtime/CMakeLists.txt` also defines the POSIX host library `cwrap_os_posix` |
| `tests/CMakeLists.txt` | Add the golden and idempotence test suites, the shared unit-test support library, the in-memory host, and the seeded random tests |
| `vendor/CMakeLists.txt` | Build each vendored dependency |
| `cmake/cwrap_build_profiles.cmake` | Set warnings, optimization, fortification, and sanitizers |
| `cmake/cwrap_quality.cmake` | Configure formatting and static analysis |
| `cmake/cwrap_testing.cmake` | Register a unit test and choose the host it links |
| `cmake/cwrap_packaging.cmake` | Configure installation and CPack archives |
| `cmake/cwrap_run_golden.cmake` | Run the golden test suite |
| `cmake/cwrap_run_idempotent.cmake` | Run the idempotence test suite |
| `cmake/cwrap_restyle_graphviz_svg.cmake` | Add light and dark styles to the target graph |
| `cmake/toolchains/` | Configure Zig for Linux `musl` targets |
| `CMakeGraphVizOptions.cmake` | Set filters and layout for the target graph |

## Target graph

`cwrap_app` is a private static library. It contains all application source files except `main.c` and `runtime/fs.c`. The `cwrap` executable and unit tests link to it. `runtime/fs.c`, the POSIX implementation of `runtime/os.h`, is the object library `cwrap_os_posix`, so each executable chooses its host when it is linked: `cwrap` and the tests registered with the default `OS posix` link it, and the tests registered with `cwrap_add_unit_test(<module> OS memory)` link `cwrap_os_memory` (`tests/os_memory.c`) instead. Both are object libraries because `cwrap_app` refers to them, and a static archive listed before its user would be skipped by a single-pass linker. `cwrap_app` links to `cwrap_vendor_sharedstuff` for the arena, counted string, output buffer, and diagnostic log implementations. The arena calls `os_oom` when an allocation does not fit, and `sharedstuff` leaves that function to the application: `main.c` defines the production one, which prints the log collected so far and a line that names the input and the phase that ran out of memory, and exits with `EXIT_CODE_FAILURE`. `cwrap_test_support` defines the unit-test one.

The library is static because it supports one product. It does not provide a public ABI.

Each vendored project has a separate target. Header-only dependencies use interface libraries.

`cwrap_app` links to `cwrap_vendor_sharedstuff` as a public dependency because application headers expose its types. The other vendored dependencies are private.

Source directories group code by function. Apart from the host implementation, they do not define separate libraries. Each local `CMakeLists.txt` adds files to `cwrap_app`. More libraries would add link boundaries without independent APIs.

### Generated dependency graph

CMake generates this graph from the `release` preset. The graph shows CMake targets and link relationships. It does not show dependencies between source modules. Graph options remove test-only targets, such as the in-memory host `cwrap_os_memory`.

![CMake target dependency graph](media/dependencies.svg)

> [!tip]
> Use these commands to update the graph:
>
> ```sh
> cmake --preset release --graphviz=build/release/target-dependencies.dot
> dot -Tsvg build/release/target-dependencies.dot \
>   -o build/release/target-dependencies.raw.svg
> cmake \
>   -DCWRAP_GRAPHVIZ_SVG_INPUT=build/release/target-dependencies.raw.svg \
>   -DCWRAP_GRAPHVIZ_SVG_OUTPUT=media/dependencies.svg \
>   -P cmake/cwrap_restyle_graphviz_svg.cmake
> ```

> [!important]
> CMake reads `CMakeGraphVizOptions.cmake` automatically when you use `--graphviz`. Do not include this file from `CMakeLists.txt`.

## Build profiles

The build uses two interface targets:

- `cwrap_build_project` sets strict first-party warnings and sanitizer options.
- `cwrap_build_tests` sets the common warning group and sanitizer options.

Both targets require C17 or later. A parent project can select a newer standard. It cannot select an older standard.

Presets use `CMAKE_COMPILE_WARNING_AS_ERROR` to treat warnings as errors. The project does not force this setting on a parent build.

Generator expressions select configuration options at build time. The same rules work with single-config and multi-config generators.

`CWRAP_SANITIZER` accepts `none` or `address`. The `address` option enables AddressSanitizer and UndefinedBehaviorSanitizer. It adds sanitizer options to standard CMake configurations. It does not create custom build types.

The address-sanitizer profile instruments first-party and vendored code. `cwrap_vendor_sharedstuff` builds with the first-party profile, so vendored code also retains the full sanitizer set.

## Linting and formatting

CMake sets `clang-tidy` and `cppcheck` as properties of first-party targets. Neither tool checks vendored sources. cppcheck reads the vendored `sharedstuff` headers to expand `STR_FMT`. It also analyzes the inline functions those headers define.

Tests run `cppcheck`. Tests do not run `clang-tidy` because deliberate failure cases cause analyzer errors.

Cross builds do not run either host analyzer. Zig supplies target headers that the host analyzers cannot find when they repeat the compile command.

The `cwrap_format` and `cwrap_lint` targets use an explicit list of first-party files. Add each new source or test to its target and this list.

This duplication is deliberate. A configure-time glob can omit a new file until CMake configures the project again.

## Tests

`CWRAP_BUILD_TESTING` controls test creation. It is true by default for a top-level build. It is false by default for a child build.

Each source directory adds its local unit tests. The top-level `tests/` directory adds the golden and idempotence test suites, `cwrap_test_support`, the in-memory host `cwrap_os_memory`, and the seeded random pass `tests/test_random.c`. Each CTest case has a `cwrap` label. It also has a `unit`, `golden`, or `idempotent` label.

`cwrap_add_unit_test` takes `OS memory` to link the in-memory host, and then compiles the test with `CWRAP_TEST_OS_MEMORY` set to 1. Every unit test registered without `OS memory` links the POSIX host: the POSIX layer's own tests and the test-support test, which need a real filesystem; the suites that build their fixtures in a real temporary directory; and the tests that reach no host. New tests use the in-memory host. `cwrap_add_unit_test` also takes `NAME` when one source builds more than one test. `src/runtime/test_os.c` uses both: it builds `cwrap.os_posix`, which works in a real temporary directory, and `cwrap.os_memory`. These are the contract tests of `runtime/os.h`. Both must pass. Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails instead of stopping the run.

`sharedstuff` has its own compile-fail tests for `buf_printf` and `log_error`. They check that a call whose arguments disagree with its format does not compile: a raw `struct Str` for `%.*s`, a missing argument, a `ptrdiff_t` for `%d`, a format that is not a literal, a non-string given to `STR_FMT`, and a disagreement through a forwarding wrapper. They run in the `sharedstuff` repository. The vendored copy holds no tests.

`cwrap_test_support` holds the unit-test plumbing that several `test_*.c` files need: a fixture-file creator, standard-stream capture and restore, an arena over a fresh reservation, a log over a fresh reservation and exact-text log checks, an out-of-memory check, and, for `tests/test_random.c`, a seeded random generator and a printable copy of any bytes for a failure message. It also defines the unit-test `os_oom`, which exits with a status of its own, so `test_is_oom` can run code in a child process and tell running out of memory apart from a crash. It is an object library, so that definition is linked into every test executable whatever the link order. Its own unit test is `tests/test_test_support.c`. `test_support.c` defines `TEST_NO_MAIN`, so acutest's `main` and run state stay in each test executable's own translation unit and the link resolves acutest's declarations, such as `acutest_check_`, `acutest_message_` and `acutest_abort_`, against it. A failure raised inside the support library is reported against `test_support.c` and fails the test that reached it.

A parent that enables `cwrap` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts discovery at the build root. A child call cannot create the root test file. CMake prints this requirement when a child enables tests.

Each unit test links the same `cwrap_app` as the executable. No test target compiles an application source a second time with different definitions.

The golden test uses a separate scratch tree. It rewraps every fixture at width 40. The CMake script then compares the fixture inventory with `tests/expected/` and compares each result with its expected file byte for byte.

The idempotence test uses another scratch tree. It rewraps the fixture and expected files and every first-party source file under `src/` and `tests/` at widths 20, 40, 60, and 100. A second pass must make no change, and `--check` must agree that the first pass is stable.

The CMake scripts collect their input files when each test runs. These runtime globs are not build inputs. Fixture changes do not require CMake to configure the project again.

## Version

CMake stores the numeric release version once in `project(VERSION)`. It generates one source file that defines `cwrap_version_string()`. A version change rebuilds this source file and relinks its targets.

Release versions use `MAJOR.MINOR.PATCH`. The `project(VERSION)` command accepts only numeric components. A prerelease suffix requires a second version value.

## Install

The install contains the executable and its documentation. Both use the `cwrap_runtime` component.

The project has no export set or development component because it does not install a library or header. `GNUInstallDirs` selects the install paths.

## Packaging

### Release packages

CPack creates packages for the different targets `cwrap-<version>-<target>.tar.gz`.

The macOS deployment target is `13.0` by default. A builder can select a newer target through the cache.

Linux `musl` builds use the two Zig toolchain files. These files select `llvm-strip` because a host strip tool cannot read the cross-built ELF files.

### Source package

CPack can also create a source package `cwrap-<version>-source.tar.gz`. The ignore list keeps CPack's repository and temporary-file exclusions. It also omits build products.
