# This file isolates third-party sources from first-party warnings and analyzers.

add_library(cwrap_vendor_acutest INTERFACE)
target_include_directories(
  cwrap_vendor_acutest SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/acutest>"
)

add_library(cwrap_vendor_copt INTERFACE)
target_include_directories(
  cwrap_vendor_copt SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/copt>"
)
