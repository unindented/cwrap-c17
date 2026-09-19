target_sources(
  cwrap_app
  PRIVATE error.c parse.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES ascii.h error.h parse.h
)

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS ascii error parse)
    cwrap_add_unit_test(${module})
  endforeach()
endif()
