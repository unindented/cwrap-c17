target_sources(
  cwrap_app
  PRIVATE atom.c comment.c doxygen.c fill.c lex.c rewrite.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES atom.h comment.h doxygen.h fill.h lex.h rewrite.h unicode_width_data.h
)

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS atom comment doxygen fill lex rewrite)
    cwrap_add_unit_test(${module})
  endforeach()
endif()
