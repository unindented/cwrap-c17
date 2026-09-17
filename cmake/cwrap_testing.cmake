# This helper registers unit tests next to their source modules.

include_guard(GLOBAL)

function(cwrap_add_unit_test module)
  set(target "cwrap_unit_${module}")
  add_executable(${target} "test_${module}.c")
  target_link_libraries(${target} PRIVATE cwrap_build_tests cwrap_vendor_acutest cwrap_app)

  add_test(NAME "cwrap.${module}" COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(
    "cwrap.${module}"
    PROPERTIES LABELS "cwrap;unit"
               ENVIRONMENT_MODIFICATION "UBSAN_OPTIONS=string_prepend:print_stacktrace=1:"
  )

  if(PROJECT_IS_TOP_LEVEL)
    cwrap_enable_test_analysis(${target})
  endif()
endfunction()
