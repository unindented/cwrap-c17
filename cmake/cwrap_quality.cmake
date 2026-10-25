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
    "${PROJECT_SOURCE_DIR}/src/app/test_wrap_input.c"
    "${PROJECT_SOURCE_DIR}/src/app/wrap_input.c"
    "${PROJECT_SOURCE_DIR}/src/app/wrap_input.h"
    "${PROJECT_SOURCE_DIR}/src/core/ascii.h"
    "${PROJECT_SOURCE_DIR}/src/core/parse.c"
    "${PROJECT_SOURCE_DIR}/src/core/parse.h"
    "${PROJECT_SOURCE_DIR}/src/core/test_ascii.c"
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
    "${PROJECT_SOURCE_DIR}/src/domain/unicode_width_data.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/fs.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/memory.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/memory.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/os.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_fs.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_memory.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/test_os.c"
    "${PROJECT_SOURCE_DIR}/tests/os_memory.c"
    "${PROJECT_SOURCE_DIR}/tests/os_memory.h"
    "${PROJECT_SOURCE_DIR}/tests/test_random.c"
    "${PROJECT_SOURCE_DIR}/tests/test_support.c"
    "${PROJECT_SOURCE_DIR}/tests/test_support.h"
    "${PROJECT_SOURCE_DIR}/tests/test_test_support.c"
)
set(cwrap_configured_c_source "${PROJECT_SOURCE_DIR}/src/app/cwrap_version.c.in")

find_program(CWRAP_CLANG_FORMAT NAMES clang-format-22 clang-format)
find_program(CWRAP_CWRAP NAMES cwrap)
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

# clang-tidy reads the compiler's command line, and it rejects GCC-only warning options such as
# `-Wlogical-op` as errors. cppcheck does not read the flags, so it runs with any compiler.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(VERBOSE "compiler is ${CMAKE_C_COMPILER_ID}, not Clang; clang-tidy is disabled")
  set(cwrap_clang_tidy_command "")
endif()

# Production and test targets run cppcheck with the same arguments. CMake hands cppcheck only the
# `-I` include directories, not the `-isystem` ones, so the vendored sharedstuff headers are named
# explicitly: without them cppcheck cannot expand `STR_FMT` and miscounts `printf` arguments.
set(cwrap_cppcheck_property
    "${cwrap_cppcheck_command};--enable=warning,performance,portability;--std=${cwrap_cppcheck_standard};--error-exitcode=1;--quiet;-I${PROJECT_SOURCE_DIR}/vendor/sharedstuff/include"
)

function(cwrap_enable_project_analysis target)
  if(cwrap_clang_tidy_command)
    set_property(
      TARGET ${target}
      PROPERTY C_CLANG_TIDY
               "${cwrap_clang_tidy_command};--quiet;--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
  endif()
  if(cwrap_cppcheck_command)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${cwrap_cppcheck_property}")
  endif()
endfunction()

function(cwrap_enable_test_analysis target)
  if(cwrap_cppcheck_command)
    set_property(TARGET ${target} PROPERTY C_CPPCHECK "${cwrap_cppcheck_property}")
  endif()
endfunction()

# The format and lint targets run clang-format and then cwrap, which refills comment prose to 100
# columns, the limit that clang-format only caps. A tool that is not installed is skipped.
if(CWRAP_CLANG_FORMAT)
  set(cwrap_clang_format_fix
      COMMAND "${CWRAP_CLANG_FORMAT}" -i ${cwrap_owned_sources}
      COMMAND "${CWRAP_CLANG_FORMAT}" -i --assume-filename=cwrap_version.c
              "${cwrap_configured_c_source}"
  )
  set(cwrap_clang_format_check
      COMMAND "${CWRAP_CLANG_FORMAT}" --dry-run --Werror ${cwrap_owned_sources}
      COMMAND "${CWRAP_CLANG_FORMAT}" --dry-run --Werror --assume-filename=cwrap_version.c
              "${cwrap_configured_c_source}"
  )
else()
  set(cwrap_clang_format_fix COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found, skipping")
  set(cwrap_clang_format_check ${cwrap_clang_format_fix})
endif()

if(CWRAP_CWRAP)
  set(cwrap_cwrap_fix
      COMMAND "${CWRAP_CWRAP}" -i ${cwrap_owned_sources} "${cwrap_configured_c_source}"
  )
  set(cwrap_cwrap_check
      COMMAND "${CWRAP_CWRAP}" --check ${cwrap_owned_sources} "${cwrap_configured_c_source}"
  )
else()
  set(cwrap_cwrap_fix COMMAND "${CMAKE_COMMAND}" -E echo "cwrap not found, skipping")
  set(cwrap_cwrap_check ${cwrap_cwrap_fix})
endif()

add_custom_target(
  cwrap_format ${cwrap_clang_format_fix} ${cwrap_cwrap_fix}
  COMMENT "Formatting first-party sources"
  COMMAND_EXPAND_LISTS VERBATIM
)
add_custom_target(
  cwrap_lint ${cwrap_clang_format_check} ${cwrap_cwrap_check}
  COMMENT "Checking first-party source formatting"
  COMMAND_EXPAND_LISTS VERBATIM
)
