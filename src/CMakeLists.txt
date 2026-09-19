# The private application library contains project code except `main()`. Unit tests link to the same
# implementation.

configure_file(
  app/cwrap_version.c.in "${CMAKE_CURRENT_BINARY_DIR}/app/cwrap_version.c" @ONLY
)

add_library(cwrap_app STATIC "${CMAKE_CURRENT_BINARY_DIR}/app/cwrap_version.c")

add_subdirectory(app)
add_subdirectory(core)
add_subdirectory(domain)
add_subdirectory(runtime)

target_link_libraries(
  cwrap_app
  PUBLIC cwrap_vendor_sharedstuff
  PRIVATE cwrap_build_project cwrap_vendor_copt
)

add_executable(cwrap app/main.c)
target_link_libraries(cwrap PRIVATE cwrap_build_project cwrap_app)

if(PROJECT_IS_TOP_LEVEL)
  cwrap_enable_project_analysis(cwrap_app)
  cwrap_enable_project_analysis(cwrap)
endif()
