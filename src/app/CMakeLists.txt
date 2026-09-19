target_sources(
  cwrap_app
  PRIVATE cli.c cli_dispatch.c cmd_wrap.c wrap_input.c
  PUBLIC FILE_SET HEADERS
         BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
         FILES cli.h cli_dispatch.h cmd_wrap.h cwrap_version.h exit_code.h wrap_input.h
)

if(CWRAP_BUILD_TESTING)
  foreach(module IN ITEMS cli cli_dispatch cmd_wrap exit_code wrap_input)
    cwrap_add_unit_test(${module})
  endforeach()
endif()
