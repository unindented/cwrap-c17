# This script rewraps every fixture. It compares each result with the expected output.

foreach(required IN ITEMS CWRAP_EXECUTABLE CWRAP_FIXTURE_DIR CWRAP_EXPECTED_DIR CWRAP_SCRATCH_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be defined")
  endif()
endforeach()

file(REMOVE_RECURSE "${CWRAP_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${CWRAP_SCRATCH_DIR}")

# Collect both file lists when the test runs. Runtime lists are not build inputs. Fixture changes do
# not require CMake to run again.
file(GLOB fixture_files RELATIVE "${CWRAP_FIXTURE_DIR}" "${CWRAP_FIXTURE_DIR}/*.c")
file(GLOB expected_files RELATIVE "${CWRAP_EXPECTED_DIR}" "${CWRAP_EXPECTED_DIR}/*.c")
list(SORT fixture_files)
list(SORT expected_files)

if(NOT fixture_files STREQUAL expected_files)
  message(FATAL_ERROR "fixture and expected file lists differ")
endif()

foreach(file IN LISTS fixture_files)
  set(actual_file "${CWRAP_SCRATCH_DIR}/${file}")
  execute_process(
    COMMAND "${CWRAP_EXECUTABLE}" -w 40 "${CWRAP_FIXTURE_DIR}/${file}"
    OUTPUT_FILE "${actual_file}"
    RESULT_VARIABLE wrap_result
  )
  if(NOT wrap_result EQUAL 0)
    message(FATAL_ERROR "fixture rewrap failed with exit status ${wrap_result}: '${file}'")
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${CWRAP_EXPECTED_DIR}/${file}" "${actual_file}"
    RESULT_VARIABLE compare_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "output differs from expected: '${file}'")
  endif()
endforeach()
