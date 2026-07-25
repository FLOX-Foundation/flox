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

# The addon compiles its own translation units and links a prebuilt archive, so
# the two halves must agree on NDEBUG. libc++ changes container layout and
# hardening between debug and release, and a mismatch does not fail to link: it
# corrupts silently. Observed symptoms of a Release addon against a Debug
# archive were SymbolRegistry::registerSymbol returning 0 for every symbol,
# malloc aborting with free_list_checksum_botch, and segfaults at exit.
#
# cmake-js builds Release by default. Read the core build's type out of its
# CMakeCache and refuse to proceed on a mismatch, naming the fix.
foreach(_flox_core_dir "${CMAKE_SOURCE_DIR}/../build" "${CMAKE_SOURCE_DIR}/../build/Release")
  if(EXISTS "${_flox_core_dir}/CMakeCache.txt")
    file(STRINGS "${_flox_core_dir}/CMakeCache.txt" _flox_core_build_type
         REGEX "^CMAKE_BUILD_TYPE:STRING=")
    string(REPLACE "CMAKE_BUILD_TYPE:STRING=" "" _flox_core_build_type "${_flox_core_build_type}")
    break()
  endif()
endforeach()

if(_flox_core_build_type)
  set(_flox_node_build_type "${CMAKE_BUILD_TYPE}")
  if(NOT _flox_node_build_type)
    set(_flox_node_build_type "Release")
  endif()
  string(TOUPPER "${_flox_core_build_type}" _core_upper)
  string(TOUPPER "${_flox_node_build_type}" _node_upper)
  # Debug is the only type without NDEBUG; Release / RelWithDebInfo /
  # MinSizeRel all define it, so they are interchangeable here.
  set(_core_ndebug ON)
  set(_node_ndebug ON)
  if(_core_upper STREQUAL "DEBUG")
    set(_core_ndebug OFF)
  endif()
  if(_node_upper STREQUAL "DEBUG")
    set(_node_ndebug OFF)
  endif()
  if(NOT _core_ndebug STREQUAL _node_ndebug)
    message(FATAL_ERROR
      "flox_node build type (${_flox_node_build_type}) disagrees with the flox "
      "archive it links (${_flox_core_build_type}). Mixing NDEBUG and non-NDEBUG "
      "halves corrupts memory instead of failing to link. Rebuild the core with "
      "-DCMAKE_BUILD_TYPE=${_flox_node_build_type}, or build the addon with "
      "npm run build -- --debug to match a Debug core.")
  endif()
endif()

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

# flox headers are feature-gated on macros the core build sets PUBLIC on the
# `flox` target. find_library() gives us the archive but none of those
# definitions, so flox_capi.cpp compiled here can disagree with the archive it
# links: FLOX_LZ4_ENABLED changes whether <lz4.h> is included and whether
# compression reports as supported, which is enough to read a tape written by
# the other half and land on corrupted state. Set them explicitly.
#
# The affinity pair is guarded because FLOX_CPU_AFFINITY_ENABLED also decides
# whether EventBus declares two data members, i.e. it changes sizeof.
# FLOX_ENABLE_CPU_AFFINITY defaults OFF in the core build; if you turn it on
# there, pass -DFLOX_NODE_CPU_AFFINITY=ON here so both halves agree.
option(FLOX_NODE_CPU_AFFINITY "Match a core build configured with FLOX_ENABLE_CPU_AFFINITY=ON" OFF)
if(FLOX_NODE_CPU_AFFINITY)
  target_compile_definitions(flox_node PRIVATE FLOX_CPU_AFFINITY_ENABLED=1)
else()
  target_compile_definitions(flox_node PRIVATE FLOX_CPU_AFFINITY_ENABLED=0)
endif()
target_compile_definitions(flox_node PRIVATE FLOX_NUMA_LIBRARY_LINKED=0)

# Mirror the LZ4 detection from the core CMakeLists. find_library(FLOX_LIB)
# locates a static archive but does not propagate the PUBLIC dependencies of
# the original `flox` target, so the binding has to re-discover lz4 itself.
# Try the three discovery methods used in the core build (vcpkg, brew/Linux
# system, pkg-config) and link whichever one matches.
set(FLOX_NODE_LZ4_FOUND OFF)
find_package(lz4 CONFIG QUIET)
if(lz4_FOUND)
  target_link_libraries(flox_node lz4::lz4)
  set(FLOX_NODE_LZ4_FOUND ON)
  message(STATUS "flox_node: LZ4 via find_package")
else()
  find_library(LZ4_LIBRARY NAMES lz4)
  find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
  if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
    target_include_directories(flox_node PRIVATE ${LZ4_INCLUDE_DIR})
    target_link_libraries(flox_node ${LZ4_LIBRARY})
    set(FLOX_NODE_LZ4_FOUND ON)
    message(STATUS "flox_node: LZ4 via find_library: ${LZ4_LIBRARY}")
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(LZ4 QUIET liblz4)
    endif()
    if(LZ4_FOUND)
      target_include_directories(flox_node PRIVATE ${LZ4_INCLUDE_DIRS})
      target_link_libraries(flox_node ${LZ4_LINK_LIBRARIES})
      set(FLOX_NODE_LZ4_FOUND ON)
      message(STATUS "flox_node: LZ4 via pkg-config")
    else()
      message(STATUS "flox_node: LZ4 not found — compressed segment reads will fail at runtime")
    endif()
  endif()
endif()

# Must match the value the linked archive was compiled with.
if(FLOX_NODE_LZ4_FOUND)
  target_compile_definitions(flox_node PRIVATE FLOX_LZ4_ENABLED=1)
else()
  target_compile_definitions(flox_node PRIVATE FLOX_LZ4_ENABLED=0)
endif()
