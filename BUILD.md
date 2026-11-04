# Build system

This document describes the targets of the `cwrap` CMake build, how the targets link to each other, and the reasons for its settings. The CMake build needs CMake 3.25 or later. The [Development](README.md#development) section of the README lists the commands that compile, test and package `cwrap`.

## Layout

The top-level [CMakeLists.txt](CMakeLists.txt) sets the project policy, includes its CMake modules, and adds each source directory. These are the other CMake files:

| Path | Responsibility |
| --- | --- |
| [src/CMakeLists.txt](src/CMakeLists.txt) | Defines the application library `cwrap_app` and the `cwrap` executable |
| `src/*/CMakeLists.txt` | Add the sources, the header file sets and the unit tests of their directory. [src/runtime/CMakeLists.txt](src/runtime/CMakeLists.txt) also defines the POSIX host `cwrap_os_posix` |
| [tests/CMakeLists.txt](tests/CMakeLists.txt) | Adds the golden and idempotence tests, the unit-test support library, the in-memory host and the seeded random tests |
| [vendor/CMakeLists.txt](vendor/CMakeLists.txt) | Defines a target for each vendored dependency |
| [cmake/cwrap_build_profiles.cmake](cmake/cwrap_build_profiles.cmake) | Sets warnings, optimization, fortification and sanitizers |
| [cmake/cwrap_quality.cmake](cmake/cwrap_quality.cmake) | Configures formatting and static analysis |
| [cmake/cwrap_testing.cmake](cmake/cwrap_testing.cmake) | Defines `cwrap_add_unit_test`, which registers a unit test and selects the host that the test links |
| [cmake/cwrap_run_golden.cmake](cmake/cwrap_run_golden.cmake) | Runs the golden test |
| [cmake/cwrap_run_idempotent.cmake](cmake/cwrap_run_idempotent.cmake) | Runs the idempotence test |
| [cmake/cwrap_packaging.cmake](cmake/cwrap_packaging.cmake) | Configures installation and CPack archives |
| [cmake/cwrap_restyle_graphviz_svg.cmake](cmake/cwrap_restyle_graphviz_svg.cmake) | Adds light and dark styles to the target graph |
| [cmake/toolchains/](cmake/toolchains/) | Configures Zig to cross-compile for the Linux `musl` release targets |
| [CMakeGraphVizOptions.cmake](CMakeGraphVizOptions.cmake) | Sets the filters and the layout of the target graph |
| [CMakePresets.json](CMakePresets.json) | Defines the configure, build, test, package and workflow presets |

## Targets

The CMake build has one application library, two hosts, one target for each vendored dependency, and the interface targets of [Build profiles](#build-profiles). Each executable links the application library and one of the two hosts. CMake generates this graph of the targets and their links from the `release` preset. The graph does not show the dependencies between source modules, and it does not show test-only targets such as the in-memory host `cwrap_os_memory`.

![CMake target dependency graph](media/dependencies.svg)

### Application library

`cwrap_app` is a private static library that holds every application source except [src/app/main.c](src/app/main.c) and `src/runtime/fs.c`. The `cwrap` executable and every unit test link it. It is static because it supports one product and has no public ABI.

Each source directory holds the code for one purpose. Except for the host in `src/runtime/`, the source directories add their files to `cwrap_app` and do not define libraries of their own, because more libraries would add link boundaries without independent APIs. `src/CMakeLists.txt` links the vendored dependencies.

`cwrap_app` links `cwrap_vendor_sharedstuff` as a `PUBLIC` dependency, because the application headers expose the types of this library. The other dependencies are `PRIVATE`.

`cwrap_vendor_sharedstuff` supplies the arena, the counted string, the output buffer and the diagnostic log. When an allocation does not fit, the arena calls `os_oom`, and each program defines its own `os_oom`. `src/app/main.c` defines the `os_oom` of `cwrap`, which [ARCHITECTURE.md](ARCHITECTURE.md#out-of-memory-exits-the-process) describes. `cwrap_test_support` defines the `os_oom` of the unit tests.

### Hosts

`src/runtime/fs.c` is the POSIX implementation of [src/runtime/os.h](src/runtime/os.h). CMake compiles this file into the object library `cwrap_os_posix`, outside `cwrap_app`, so each executable selects its host when it links. `cwrap` links `cwrap_os_posix`. The unit tests that use the default, `OS posix`, also link it (see [Unit tests](#unit-tests)). The tests registered with `OS memory` link `cwrap_os_memory`, the in-memory host in [tests/os_memory.c](tests/os_memory.c), instead.

Both hosts are object libraries, because `cwrap_app` refers to them and a single-pass linker skips a static archive when the archive comes before the code that uses it on the link line.

### Vendored dependencies

Each vendored project has its own target: the header-only projects, acutest and copt, are interface libraries, and `sharedstuff` is a static library.

### Regenerating the graph

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

Two interface targets carry the compile and link options:

- `cwrap_build_project` sets the strict warnings for first-party code and the sanitizer options.
- `cwrap_build_tests` sets the common warning group and the sanitizer options.

Both targets require C17 or later. A parent project can select a newer standard, but not an older one.

Generator expressions select the options separately for each configuration, so the same rules work with single-config generators and with multi-config generators.

The presets set `CMAKE_COMPILE_WARNING_AS_ERROR`, so the compiler treats warnings as errors. The CMake build does not force this setting on a parent project.

### Sanitizers

`CWRAP_SANITIZER` accepts `none` or `address`. `address` enables AddressSanitizer and UndefinedBehaviorSanitizer. The option adds flags to the standard CMake configurations and does not create custom build types.

With `address`, the CMake build instruments first-party code and vendored code. `cwrap_vendor_sharedstuff` builds with the first-party profile, so vendored code also keeps every check.

## Linting and formatting

When this repository is the top-level CMake project, the `C_CLANG_TIDY` and `C_CPPCHECK` properties of the first-party targets make CMake run `clang-tidy` and `cppcheck` while it compiles these targets. Neither tool checks vendored code. The include path of `cppcheck` contains the vendored `sharedstuff` headers, because `cppcheck` needs these headers to expand `STR_FMT`. Thus `cppcheck` also analyzes the inline functions that these headers define.

Test targets run `cppcheck` but not `clang-tidy`, because their intentional failure cases cause analyzer errors. When CMake cross-compiles, neither tool runs. Zig supplies the headers for the target platform, and the analyzers on the host cannot find these headers when they repeat the compile command. When the compiler is not Clang, the CMake build turns off `clang-tidy`, because `clang-tidy` reads the compile command and rejects GCC-only warning options such as `-Wlogical-op`. `cppcheck` runs with any compiler.

The `cwrap_format` and `cwrap_lint` targets run `clang-format` and then `cwrap`, which refills comment prose to 100 columns, the `.clang-format` column limit. Either tool is skipped when it is not installed. CI and the Linux image install a pinned `cwrap` release. It is the installed `cwrap`, not the one this tree builds, so a change to how it rewraps reaches these sources only through a new release and the pin in CI and the Linux image.

Both targets run both tools on the first-party files in an explicit list, `cwrap_owned_sources` in `cmake/cwrap_quality.cmake`. They also run both tools on the template [src/app/cwrap_version.c.in](src/app/cwrap_version.c.in), which `clang-format` reads as `cwrap_version.c` because of `--assume-filename`. When you add a source file or a test file, add it to this list and to its target. A glob at configure time would not find a new file until CMake configures the project again.

## Tests

`CWRAP_BUILD_TESTING` controls whether CMake defines the tests. It is on by default when this repository is the top-level CMake project, and off by default when a parent project adds this repository as a subproject.

Each source directory adds its own unit tests. `tests/` adds the golden and idempotence tests, `cwrap_test_support`, `cwrap_os_memory` and the seeded random tests. Every CTest case has the label `cwrap`, and also the label `unit`, `golden` or `idempotent`.

### Unit tests

`cwrap_add_unit_test(<module>)` compiles `test_<module>.c` into the executable `cwrap_unit_<module>`, and registers this executable as the CTest case `cwrap.<module>`. The executable links `cwrap_build_tests`, acutest, `cwrap_test_support`, `cwrap_app` and one host.

With the `OS memory` option, `cwrap_add_unit_test` links the in-memory host and compiles the test with `CWRAP_TEST_OS_MEMORY` set to 1. New tests use this option. The tests registered without `OS memory` link the POSIX host. These tests are:

- the tests that need a real filesystem: `fs` and `os_posix`, which test the POSIX layer; and `test_support`, which tests the test-support library
- the suites that are not yet converted to the in-memory host, which create their fixtures in a real temporary directory: `cli` and `cli_dispatch`
- the tests that use no host

When one source file compiles into more than one executable, the `NAME` option sets the name of each executable and its test. [src/runtime/test_os.c](src/runtime/test_os.c) uses both options. It compiles into two tests: `cwrap.os_posix`, which works in a real temporary directory, and `cwrap.os_memory`. These tests are the contract tests of `src/runtime/os.h`, and both tests must pass.

Each unit test has a CTest timeout of 60 seconds, so a test that hangs fails and does not block the test run.

No test target compiles an application source file a second time with different definitions.

### Test-support library

`cwrap_test_support` holds the helpers that several `test_*.c` files need:

- functions that create a fixture root, create a fixture file and remove a fixture tree recursively
- functions that capture and restore the standard streams
- functions that make an arena or a log over a fresh reservation, and checks that compare the text of a log exactly
- an out-of-memory check, `test_is_oom`
- for [tests/test_random.c](tests/test_random.c), a seeded random generator, and a function that makes a printable copy of any bytes for a failure message

The library also defines the `os_oom` of the unit tests. This `os_oom` exits with its own exit status, so `test_is_oom` can run code in a child process and tell an out-of-memory exit apart from a crash. The library is an object library, so that this definition links into every test executable in any link order. The unit test of the library is [tests/test_test_support.c](tests/test_test_support.c).

[tests/test_support.c](tests/test_support.c) defines `TEST_NO_MAIN`, so the `main` function and the run state of acutest stay in the translation unit of each test executable. The library uses acutest declarations such as `acutest_check_`, `acutest_message_` and `acutest_abort_`, and the linker resolves these declarations against that translation unit. When a check in the library fails, acutest reports the failure against `test_support.c`, and the test that called the library fails.

### Golden test

The golden test `cwrap.golden` runs `cmake/cwrap_run_golden.cmake`. The script rewraps every fixture in `tests/fixtures/` at width 40, in a scratch tree of its own. It then compares the list of fixture files with the list of files in `tests/expected/`, and it compares each result with its expected file byte for byte.

### Idempotence test

The idempotence test `cwrap.idempotent` runs `cmake/cwrap_run_idempotent.cmake` in another scratch tree. The script rewraps the fixture files, the expected files and every first-party source file in `src/` and `tests/` at widths 20, 40, 60 and 100. A second rewrap of each result must make no change, and `--check` must agree that the first result is stable.

Both scripts find their input files with globs. These globs run in the test and are not inputs of the CMake build, so a change to a fixture or a source file does not require CMake to configure the project again.

### Format checks in sharedstuff

`sharedstuff` has its own compile-fail tests for `buf_printf` and `log_error`. These tests check that a call does not compile when its arguments do not match its format. The test cases are:

- a raw `struct Str` for `%.*s`
- a missing argument
- a `ptrdiff_t` for `%d`
- a format that is not a literal
- a value that is not a string, given to `STR_FMT`
- a mismatch through a forwarding wrapper

These tests run in the `sharedstuff` repository. The vendored copy holds no tests.

### Tests in a parent build

A parent project that turns on the `cwrap` tests must call `enable_testing()` in its top-level `CMakeLists.txt`. CTest starts to find tests at the root of the build directory, and a call to `enable_testing()` in a subproject cannot create the test file at that root. When a subproject turns on its tests, CMake prints this requirement.

## Releases

### Version

`project(VERSION)` in `CMakeLists.txt` holds the only copy of the numeric release version. CMake generates one source file from `src/app/cwrap_version.c.in`, and this file defines `cwrap_version_string()`. When the version changes, CMake compiles this file again and links the targets that use it again.

Release versions have the form `MAJOR.MINOR.PATCH`. `project(VERSION)` accepts only numeric components, so a prerelease suffix needs a second version value.

### Install

The installation contains the executable and its documentation files, `README.md`, `LICENSE.txt` and `THIRD_PARTY_NOTICES.md`. All of these files are in the `cwrap_runtime` component. `GNUInstallDirs` selects the install paths. The project installs no library and no header, so it has no export set and no development component.

### Packages

CPack creates one stripped archive for each release target, `cwrap-<version>-<target>.tar.gz`. The cache variable `CWRAP_RELEASE_TARGET` names the release target, for example `x86_64-linux-musl` in a Linux preset.

The macOS deployment target, `CMAKE_OSX_DEPLOYMENT_TARGET`, is `13.0` by default. You can set a higher value in the CMake cache.

The Linux `musl` presets cross-compile with the Zig toolchain files in `cmake/toolchains/`. These files select `llvm-strip`, because the strip tool of the host cannot read the cross-compiled ELF files.

The source archive, `cwrap-<version>-source.tar.gz`, keeps the default CPack exclusions for version-control directories and temporary files. It also leaves out `build/` and `CMakeUserPresets.json`.
