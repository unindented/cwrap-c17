target_sources(
  cwrap_app
  PRIVATE arena.c error.c parse.c string_buffer.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES arena.h ascii.h error.h parse.h string_buffer.h
)

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS arena ascii error parse string_buffer)
    cwrap_add_unit_test(${module})
  endforeach()
endif()
