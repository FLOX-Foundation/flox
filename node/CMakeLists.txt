cmake_minimum_required(VERSION 3.15)
project(flox_node)

set(CMAKE_CXX_STANDARD 23)

# node-addon-api
execute_process(
  COMMAND node -p "require('node-addon-api').include_dir"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NAPI_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE)

# Node.js headers (provided by cmake-js)
include_directories(${CMAKE_JS_INC} ${NAPI_INCLUDE_DIR})

# flox headers + capi
include_directories(${CMAKE_SOURCE_DIR}/../include)
include_directories(${CMAKE_SOURCE_DIR}/../src/capi)

# Find the built flox static library (Release/ subdir for MSVC multi-config generators)
find_library(FLOX_LIB flox HINTS
  ${CMAKE_SOURCE_DIR}/../build
  ${CMAKE_SOURCE_DIR}/../build/Release)

# Compile flox_capi.cpp directly to avoid DLL/import-lib complexity on Windows
add_library(flox_node SHARED
  src/flox_node.cpp
  ${CMAKE_SOURCE_DIR}/../src/capi/flox_capi.cpp
  ${CMAKE_JS_SRC})
set_target_properties(flox_node PROPERTIES PREFIX "" SUFFIX ".node")
# Match the runtime library used by flox.lib (/MD) — cmake-js defaults to /MT
if(MSVC)
  set_property(TARGET flox_node PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
target_link_libraries(flox_node ${CMAKE_JS_LIB} ${FLOX_LIB})
target_compile_definitions(flox_node PRIVATE NAPI_VERSION=8 NODE_ADDON_API_DISABLE_DEPRECATED)
