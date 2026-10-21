# The golden and idempotence test suites use several modules. Keep them in the top-level `tests/`
# directory.

# Unit-test plumbing shared by every unit-test executable. It is an object library rather than a
# static one so that its `os_oom` is always linked in: a static archive member would be pulled only
# by a reference that precedes it on the link line, and the reference comes from the arena in
# `cwrap_vendor_sharedstuff`, which follows it.
add_library(cwrap_test_support OBJECT test_support.c)
target_include_directories(cwrap_test_support PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(cwrap_test_support PRIVATE cwrap_build_tests cwrap_vendor_acutest cwrap_app)
if(PROJECT_IS_TOP_LEVEL)
  cwrap_enable_test_analysis(cwrap_test_support)
endif()
cwrap_add_unit_test(test_support)

# The in-memory implementation of `src/runtime/os.h`, which a unit test registered with `OS memory`
# links in place of the POSIX one. It is an object library for the same reason as the POSIX one.
add_library(cwrap_os_memory OBJECT os_memory.c)
target_include_directories(cwrap_os_memory PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>")
target_link_libraries(cwrap_os_memory PRIVATE cwrap_build_tests cwrap_vendor_acutest cwrap_app)
if(PROJECT_IS_TOP_LEVEL)
  cwrap_enable_test_analysis(cwrap_os_memory)
endif()

# Seeded random tests over the rewrap.
cwrap_add_unit_test(random)

add_test(
  NAME cwrap.golden
  COMMAND
    "${CMAKE_COMMAND}" "-DCWRAP_EXECUTABLE=$<TARGET_FILE:cwrap>"
    "-DCWRAP_FIXTURE_DIR=${CMAKE_CURRENT_LIST_DIR}/fixtures"
    "-DCWRAP_EXPECTED_DIR=${CMAKE_CURRENT_LIST_DIR}/expected"
    "-DCWRAP_SCRATCH_DIR=${PROJECT_BINARY_DIR}/golden/$<CONFIG>" -P
    "${PROJECT_SOURCE_DIR}/cmake/cwrap_run_golden.cmake"
)
set_tests_properties(
  cwrap.golden
  PROPERTIES LABELS "cwrap;golden"
             ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
)

add_test(
  NAME cwrap.idempotent
  COMMAND
    "${CMAKE_COMMAND}" "-DCWRAP_EXECUTABLE=$<TARGET_FILE:cwrap>"
    "-DCWRAP_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DCWRAP_SCRATCH_DIR=${PROJECT_BINARY_DIR}/idempotent/$<CONFIG>" -P
    "${PROJECT_SOURCE_DIR}/cmake/cwrap_run_idempotent.cmake"
)
set_tests_properties(
  cwrap.idempotent
  PROPERTIES LABELS "cwrap;idempotent"
             ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
)
