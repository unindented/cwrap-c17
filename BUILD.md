# Build system

`cwrap` requires CMake 3.25 or later. This document explains the build targets and their relationships.

## Layout

The top-level CMake file controls the build. It sets project policy, loads build settings, and adds each source directory.

| Path | Responsibility |
| --- | --- |
| `src/CMakeLists.txt` | Build the private application library and the `cwrap` executable |
| `src/*/CMakeLists.txt` | Add sources, header file sets, and local unit tests |
| `tests/CMakeLists.txt` | Add the golden and idempotence test suites |
| `vendor/CMakeLists.txt` | Build each vendored dependency |
| `cmake/cwrap_build_profiles.cmake` | Set warnings, optimization, fortification, and sanitizers |
| `cmake/cwrap_quality.cmake` | Configure formatting and static analysis |
| `cmake/cwrap_testing.cmake` | Add local unit tests |
| `cmake/cwrap_packaging.cmake` | Configure installation and CPack archives |
| `cmake/cwrap_run_golden.cmake` | Run the golden test suite |
| `cmake/cwrap_run_idempotent.cmake` | Run the idempotence test suite |
| `cmake/cwrap_restyle_graphviz_svg.cmake` | Add light and dark styles to the target graph |
| `cmake/toolchains/` | Configure Zig for Linux `musl` targets |
| `CMakeGraphVizOptions.cmake` | Set filters and layout for the target graph |

## Target graph

`cwrap_app` is a private static library. It contains all first-party source files except `main.c`. The `cwrap` executable links to this library. Unit tests link to the same library.

The library is static because it supports one product. It does not provide a public ABI.

Each vendored project has a separate target. Header-only dependencies use interface libraries.

Source directories group code by function. They do not define separate libraries. Each local `CMakeLists.txt` adds files to `cwrap_app`. More libraries would add link boundaries without independent APIs.

### Generated dependency graph

CMake generates this graph from the `Release` configuration. The graph shows CMake targets and link relationships. It does not show dependencies between source modules. Graph options remove test-only targets.

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

`CWRAP_SANITIZER` accepts `none`, `address`, or `thread`. It adds sanitizer options to standard CMake configurations. It does not create custom build types.

## Linting and formatting

CMake sets `clang-tidy` and `cppcheck` as properties of first-party targets. Neither tool checks vendored code.

Tests run `cppcheck`. Tests do not run `clang-tidy` because deliberate failure cases cause analyzer errors.

Cross builds do not run either host analyzer. Zig supplies target headers that the host analyzers cannot find when they repeat the compile command.

The `cwrap_format` and `cwrap_lint` targets use an explicit list of first-party files. Add each new source or test to its target and this list.

This duplication is deliberate. A configure-time glob can omit a new file until CMake configures the project again.

## Tests

`CWRAP_BUILD_TESTING` controls test creation. It is true by default for a top-level build. It is false by default for a child build.

Each source directory adds its local unit tests. The top-level `tests/` directory adds only the golden and idempotence test suites. Each CTest case has a `cwrap` label. It also has a `unit`, `golden`, or `idempotent` label.

A parent that enables `cwrap` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts discovery at the build root. A child call cannot create the root test file. CMake prints this requirement when a child enables tests.

The golden test uses a separate scratch tree. Its CMake script collects fixture and expected files after `cwrap` runs. These runtime globs are not build inputs. Fixture changes do not require CMake to configure the project again.

The idempotence test uses another scratch tree. It rewraps fixture, expected, and first-party source files at widths 20, 40, 60, and 100. A second pass must make no change, and `--check` must agree that the first pass is stable.

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
