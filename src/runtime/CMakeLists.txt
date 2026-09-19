target_sources(
  cwrap_app
  PRIVATE fs.c stream.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES fs.h stream.h
)

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS fs stream)
    cwrap_add_unit_test(${module})
  endforeach()
endif()
