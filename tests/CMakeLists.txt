# The golden and idempotence test suites use several modules. Keep them in the top-level `tests/`
# directory.

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
