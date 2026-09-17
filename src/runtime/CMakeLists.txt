target_sources(
  cwrap_app
  PRIVATE fs.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES fs.h
)

if(CWRAP_BUILD_TESTING)
  cwrap_add_unit_test(fs)
endif()
