cmake_minimum_required(VERSION 3.14)

set(WEBVIEW2_VERSION "1.0.3650.58")

project(sharpblue_library LANGUAGES CXX)
cmake_policy(VERSION 3.14...3.25)

set(PLUGIN_NAME "sharpblue_library_plugin")

# ---------------------------------------------------------------------------
# NuGet: fetch WebView2 into the shared build packages dir.
# If flutter_inappwebview_windows already ran nuget install for the same
# version, NuGet returns immediately from cache — no redundant download.
# ---------------------------------------------------------------------------
find_program(NUGET nuget)
if(NOT NUGET)
  message(WARNING "nuget.exe not found — html_view requires it to fetch WebView2 SDK")
endif()

add_custom_target(sharpblue_library_DEPS ALL)
add_custom_command(
  TARGET sharpblue_library_DEPS PRE_BUILD
  COMMAND ${NUGET} install Microsoft.Web.WebView2
          -Version ${WEBVIEW2_VERSION}
          -ExcludeVersion
          -OutputDirectory ${CMAKE_BINARY_DIR}/packages
  DEPENDS ${NUGET}
)

# ---------------------------------------------------------------------------
# Plugin sources
# ---------------------------------------------------------------------------
add_library(${PLUGIN_NAME} SHARED
  "include/html_view/html_view_plugin_c_api.h"
  "html_view_plugin_c_api.cpp"
  "html_view_plugin.h"
  "html_view_plugin.cpp"
  "webview2_view.h"
  "webview2_view.cpp"
)

add_dependencies(${PLUGIN_NAME} sharpblue_library_DEPS)

target_compile_features(${PLUGIN_NAME} PUBLIC cxx_std_17)
target_compile_options(${PLUGIN_NAME} PRIVATE /W3 /EHsc)
target_compile_definitions(${PLUGIN_NAME} PRIVATE
  FLUTTER_PLUGIN_IMPL
  UNICODE
  _UNICODE
  "_HAS_EXCEPTIONS=0"
  "$<$<CONFIG:Debug>:_DEBUG>"
)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin comctl32)

# WebView2 via the shared NuGet packages dir
target_link_libraries(${PLUGIN_NAME} PRIVATE
  "${CMAKE_BINARY_DIR}/packages/Microsoft.Web.WebView2/build/native/Microsoft.Web.WebView2.targets")

set(sharpblue_library_bundled_libraries "" PARENT_SCOPE)
