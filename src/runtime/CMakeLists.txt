target_sources(
  cwrap_app
  PRIVATE memory.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES memory.h os.h
)

# The POSIX implementation of `os.h` stays out of `cwrap_app`, so each executable chooses its host
# when it is linked: the `cwrap` executable and every unit test registered without `OS memory` link
# this one, and the tests registered with it link the in-memory one from `tests/`. It is an object
# library because `cwrap_app` refers to it, and a static archive listed before its user would be
# skipped by a single-pass linker.
add_library(cwrap_os_posix OBJECT fs.c)
target_sources(
  cwrap_os_posix PUBLIC FILE_SET HEADERS BASE_DIRS "${PROJECT_SOURCE_DIR}/src" FILES fs.h
)
target_link_libraries(cwrap_os_posix PRIVATE cwrap_build_project cwrap_app)
if(PROJECT_IS_TOP_LEVEL)
  cwrap_enable_project_analysis(cwrap_os_posix)
endif()

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS fs)
    cwrap_add_unit_test(${module})
  endforeach()
  cwrap_add_unit_test(memory OS memory)
  # The contract tests of `os.h`: one source, run against each host.
  cwrap_add_unit_test(os NAME os_posix)
  cwrap_add_unit_test(os NAME os_memory OS memory)
endif()
