# This script rewraps every fixture. It compares each result with the expected output. The release
# workflow runs it against a packaged binary, with `CWRAP_EMULATOR` naming a launcher such as
# `qemu-aarch64` when the binary targets another architecture.

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

set(missing_files ${expected_files})
if(fixture_files)
  list(REMOVE_ITEM missing_files ${fixture_files})
endif()
if(missing_files)
  list(JOIN missing_files "', '" missing_files_display)
  message(FATAL_ERROR "expected output has no fixture: '${missing_files_display}'")
endif()

set(extra_files ${fixture_files})
if(expected_files)
  list(REMOVE_ITEM extra_files ${expected_files})
endif()
if(extra_files)
  list(JOIN extra_files "', '" extra_files_display)
  message(FATAL_ERROR "fixture has no expected output: '${extra_files_display}'")
endif()

foreach(file IN LISTS fixture_files)
  set(actual_file "${CWRAP_SCRATCH_DIR}/${file}")
  execute_process(
    COMMAND ${CWRAP_EMULATOR} "${CWRAP_EXECUTABLE}" -w 40 "${CWRAP_FIXTURE_DIR}/${file}"
    OUTPUT_FILE "${actual_file}"
    RESULT_VARIABLE wrap_result
    ERROR_VARIABLE wrap_stderr
  )
  if(NOT wrap_result EQUAL 0)
    message(
      FATAL_ERROR "fixture rewrap failed with exit status ${wrap_result}: '${file}'\n${wrap_stderr}"
    )
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
