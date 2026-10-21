# This helper registers a unit test built from `test_<module>.c` in the calling directory.

include_guard(GLOBAL)

# `OS` names the implementation of `src/runtime/os.h` the test links: `posix` (the default) or
# `memory` for the in-memory host in `tests/os_memory.c`. Every test registered without `OS memory`
# links the POSIX host: the tests that need the real host, such as the POSIX layer's own tests and
# the test-support test; the suites written before the in-memory host, which stay on it until they
# are converted; and the tests that reach no host. New tests use the in-memory host. A test that
# links the in-memory host is compiled with `CWRAP_TEST_OS_MEMORY` set to 1. `NAME` names the
# executable and the test when one source builds more than one, as the `os` contract tests do. It
# defaults to `module`.
function(cwrap_add_unit_test module)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "OS;NAME" "")
  if(NOT arg_OS)
    set(arg_OS posix)
  endif()
  if(NOT arg_NAME)
    set(arg_NAME "${module}")
  endif()
  set(target "cwrap_unit_${arg_NAME}")
  add_executable(${target} "test_${module}.c")
  target_link_libraries(
    ${target} PRIVATE cwrap_build_tests cwrap_vendor_acutest cwrap_test_support cwrap_app
                      cwrap_os_${arg_OS}
  )
  if(arg_OS STREQUAL "memory")
    target_compile_definitions(${target} PRIVATE CWRAP_TEST_OS_MEMORY=1)
  endif()

  add_test(NAME "cwrap.${arg_NAME}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  # A unit test that hangs fails after this many seconds instead of stopping the whole run. That is
  # several times the slowest unit test in an 8-job ASan run on an Apple M1 Max, which took under
  # 10 seconds.
  set_tests_properties(
    "cwrap.${arg_NAME}"
    PROPERTIES LABELS "cwrap;unit"
               TIMEOUT 60
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )

  if(PROJECT_IS_TOP_LEVEL)
    cwrap_enable_test_analysis(${target})
  endif()
endfunction()
