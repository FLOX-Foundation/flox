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

# Mirror the LZ4 detection from the core CMakeLists. find_library(FLOX_LIB)
# locates a static archive but does not propagate the PUBLIC dependencies of
# the original `flox` target, so the binding has to re-discover lz4 itself.
# Try the three discovery methods used in the core build (vcpkg, brew/Linux
# system, pkg-config) and link whichever one matches.
find_package(lz4 CONFIG QUIET)
if(lz4_FOUND)
  target_link_libraries(flox_node lz4::lz4)
  message(STATUS "flox_node: LZ4 via find_package")
else()
  find_library(LZ4_LIBRARY NAMES lz4)
  find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
  if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
    target_include_directories(flox_node PRIVATE ${LZ4_INCLUDE_DIR})
    target_link_libraries(flox_node ${LZ4_LIBRARY})
    message(STATUS "flox_node: LZ4 via find_library: ${LZ4_LIBRARY}")
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LZ4 QUIET liblz4)
    endif()
    if(LZ4_FOUND)
      target_include_directories(flox_node PRIVATE ${LZ4_INCLUDE_DIRS})
      target_link_libraries(flox_node ${LZ4_LINK_LIBRARIES})
      message(STATUS "flox_node: LZ4 via pkg-config")
    else()
      message(STATUS "flox_node: LZ4 not found — compressed segment reads will fail at runtime")
    endif()
  endif()
endif()
