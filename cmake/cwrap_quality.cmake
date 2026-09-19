# This file configures formatting and static analysis for first-party sources.

include_guard(GLOBAL)

set(cwrap_owned_sources
    "${PROJECT_SOURCE_DIR}/src/app/cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli.h"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/cli_dispatch.h"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_wrap.c"
    "${PROJECT_SOURCE_DIR}/src/app/cmd_wrap.h"
    "${PROJECT_SOURCE_DIR}/src/app/cwrap_version.h"
    "${PROJECT_SOURCE_DIR}/src/app/exit_code.h"
    "${PROJECT_SOURCE_DIR}/src/app/main.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cli_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_cmd_wrap.c"
    "${PROJECT_SOURCE_DIR}/src/app/test_exit_code.c"
    "${PROJECT_SOURCE_DIR}/src/core/ascii.h"
    "${PROJECT_SOURCE_DIR}/src/core/error.c"
    "${PROJECT_SOURCE_DIR}/src/core/error.h"
    "${PROJECT_SOURCE_DIR}/src/core/parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/parse.h"
    "${PROJECT_SOURCE_DIR}/src/core/test_ascii.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_error.c"
    "${PROJECT_SOURCE_DIR}/src/core/test_parse.c"
    "${PROJECT_SOURCE_DIR}/src/domain/atom.c"
    "${PROJECT_SOURCE_DIR}/src/domain/atom.h"
    "${PROJECT_SOURCE_DIR}/src/domain/comment.c"
    "${PROJECT_SOURCE_DIR}/src/domain/comment.h"
    "${PROJECT_SOURCE_DIR}/src/domain/doxygen.c"
    "${PROJECT_SOURCE_DIR}/src/domain/doxygen.h"
    "${PROJECT_SOURCE_DIR}/src/domain/fill.c"
    "${PROJECT_SOURCE_DIR}/src/domain/fill.h"
    "${PROJECT_SOURCE_DIR}/src/domain/lex.c"
    "${PROJECT_SOURCE_DIR}/src/domain/lex.h"
    "${PROJECT_SOURCE_DIR}/src/domain/rewrite.c"
    "${PROJECT_SOURCE_DIR}/src/domain/rewrite.h"
    "${PROJECT_SOURCE_DIR}/src/domain/test_atom.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_comment.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_doxygen.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_fill.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_lex.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_rewrite.c"
    "${PROJECT_SOURCE_DIR}/src/domain/test_wrap_file.c"
    "${PROJECT_SOURCE_DIR}/src/domain/unicode_width_data.h"
    "${PROJECT_SOURCE_DIR}/src/domain/wrap_file.c"
    "${PROJECT_SOURCE_DIR}/src/domain/wrap_file.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_fs.c"
)
set(cwrap_configured_c_source "${PROJECT_SOURCE_DIR}/src/app/cwrap_version.c.in")

find_program(CWRAP_CLANG_FORMAT NAMES clang-format-22 clang-format)
find_program(CWRAP_CLANG_TIDY NAMES clang-tidy-22 clang-tidy)
find_program(CWRAP_CPPCHECK NAMES cppcheck)

set(cwrap_clang_tidy_command "${CWRAP_CLANG_TIDY}")
set(cwrap_cppcheck_command "${CWRAP_CPPCHECK}")
set(cwrap_cppcheck_standard "c17")
if(CMAKE_CROSSCOMPILING)
  message(VERBOSE "cross compiling; clang-tidy and cppcheck are disabled")
  set(cwrap_clang_tidy_command "")
  set(cwrap_cppcheck_command "")
endif()

function(cwrap_enable_project_analysis target)
  if(cwrap_clang_tidy_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CLANG_TIDY
               "${cwrap_clang_tidy_command};--quiet;--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
  endif()
  if(cwrap_cppcheck_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CPPCHECK
               "${cwrap_cppcheck_command};--enable=warning,performance,portability;--std=${cwrap_cppcheck_standard};--error-exitcode=1;--quiet"
    )
  endif()
endfunction()

function(cwrap_enable_test_analysis target)
  if(cwrap_cppcheck_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CPPCHECK
               "${cwrap_cppcheck_command};--enable=warning,performance,portability;--std=${cwrap_cppcheck_standard};--error-exitcode=1;--quiet"
    )
  endif()
endfunction()

if(CWRAP_CLANG_FORMAT)
  add_custom_target(
    cwrap_format
    COMMAND "${CWRAP_CLANG_FORMAT}" -i ${cwrap_owned_sources}
    COMMAND
      "${CWRAP_CLANG_FORMAT}" -i --assume-filename=cwrap_version.c
      "${cwrap_configured_c_source}"
    COMMENT "Formatting first-party sources"
    COMMAND_EXPAND_LISTS VERBATIM
  )
  add_custom_target(
    cwrap_lint
    COMMAND "${CWRAP_CLANG_FORMAT}" --dry-run --Werror ${cwrap_owned_sources}
    COMMAND
      "${CWRAP_CLANG_FORMAT}" --dry-run --Werror --assume-filename=cwrap_version.c
      "${cwrap_configured_c_source}"
    COMMENT "Checking first-party source formatting"
    COMMAND_EXPAND_LISTS VERBATIM
  )
else()
  add_custom_target(cwrap_format COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
  add_custom_target(cwrap_lint COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found; skipping")
endif()
