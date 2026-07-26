cmake_minimum_required(VERSION 3.14)
set(PLUGIN_NAME "sharpblue_library_plugin")

project(${PLUGIN_NAME} LANGUAGES CXX)
cmake_policy(VERSION 3.14...3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK  REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)

add_library(${PLUGIN_NAME} SHARED
  "html_view_plugin.cc"
  "webkit_view.cc"
)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_VISIBILITY_PRESET hidden)

target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK PkgConfig::WEBKIT)
