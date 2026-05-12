cmake_minimum_required(VERSION 3.22)
project(flox VERSION 0.6.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FLOX_ENABLE_DEV_SETUP "Install pre-commit hook" OFF)

if(FLOX_ENABLE_DEV_SETUP)
  message(STATUS "Developer setup enabled")

  set(PRECOMMIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pre-commit")
  set(PRECOMMIT_DST "${CMAKE_CURRENT_SOURCE_DIR}/.git/hooks/pre-commit")

  if(EXISTS "${PRECOMMIT_SRC}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PRECOMMIT_SRC}" "${PRECOMMIT_DST}")
    execute_process(COMMAND chmod +x "${PRECOMMIT_DST}")
    message(STATUS "Installed pre-commit hook to .git/hooks")
  else()
    message(WARNING "pre-commit script not found at ${PRECOMMIT_SRC}")
  endif()
endif()

include_directories(include)

file(GLOB_RECURSE SRC_FILES
    src/aggregator/*.cpp
    src/engine/*.cpp
    src/execution/*.cpp
    src/log/*.cpp
    src/replay/*.cpp
    src/report/*.cpp
    src/risk/*.cpp
    src/run/*.cpp
)

option(FLOX_ENABLE_BACKTEST "Enable backtest module" OFF)
if(FLOX_ENABLE_BACKTEST)
  file(GLOB BACKTEST_SRC_FILES src/backtest/*.cpp)
  list(APPEND SRC_FILES ${BACKTEST_SRC_FILES})
  message(STATUS "Backtest module enabled")
endif()

set(FLOX ${PROJECT_NAME})

add_library(${FLOX} STATIC ${SRC_FILES})
add_library(flox::${FLOX} ALIAS ${FLOX})

# Platform-specific settings
if(WIN32)
  # Prevent Windows.h from defining min/max macros that break std::min/max
  target_compile_definitions(${FLOX} PUBLIC NOMINMAX)
endif()

# FLOX_NATIVE controls whether Release builds target the build machine's
# instruction set (`-march=native`) or a portable baseline. `native` is
# fastest but ties the resulting binary to the build host — if the build
# CPU has AVX-512 and the runtime CPU does not, the process dies with
# SIGILL on the first vector instruction. Default ON preserves the
# historical local-dev behaviour; wheel/binary distribution paths
# (python/pyproject.toml) flip it OFF.
option(FLOX_NATIVE
  "Compile Release with -march=native. Disable for distributable artifacts."
  ON)

if(NOT MSVC)
  if(FLOX_NATIVE)
    set(FLOX_ARCH_OPT -march=native)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # x86-64-v3 = AVX2 + BMI2 + FMA, baseline for Haswell/Excavator
    # (~2015) and up. Covers the vast majority of distribution targets
    # without requiring AVX-512.
    set(FLOX_ARCH_OPT -march=x86-64-v3)
  else()
    # arm64/aarch64 etc.: compiler default is portable enough; macOS
    # arm64 wheels are arch-tagged anyway so native vs. baseline is moot.
    set(FLOX_ARCH_OPT "")
  endif()
endif()

if(MSVC)
  target_compile_options(${FLOX} PUBLIC
    $<$<CONFIG:Release>:/O2 /GL>
  )
  target_link_options(${FLOX} PUBLIC
    $<$<CONFIG:Release>:/LTCG>
  )
else()
  set(FLOX_RELEASE_OPTS -O3 -flto -funroll-loops)
  if(FLOX_ARCH_OPT)
    list(APPEND FLOX_RELEASE_OPTS ${FLOX_ARCH_OPT})
  endif()
  target_compile_options(${FLOX} PUBLIC
    $<$<CONFIG:Release>:${FLOX_RELEASE_OPTS}>
  )
  # LTO requires -flto flag at link time as well
  target_link_options(${FLOX} PUBLIC
    $<$<CONFIG:Release>:-flto>
  )
endif()

# WARNING: CPU affinity can decrease performance on busy/shared systems.
# It prevents OS scheduler optimization and should only be used on isolated,
# dedicated hardware where you control the entire system workload.
option(FLOX_ENABLE_CPU_AFFINITY "Enable CPU affinity and NUMA functionality" OFF)
if(FLOX_ENABLE_CPU_AFFINITY)
  message(STATUS "CPU affinity features enabled")
  target_compile_definitions(${FLOX} PUBLIC FLOX_CPU_AFFINITY_ENABLED=1)
  # Find and conditionally link NUMA library for CPU affinity functionality
  find_library(NUMA_LIBRARY numa)
  if(NUMA_LIBRARY)
    target_link_libraries(${FLOX} PUBLIC ${NUMA_LIBRARY})
    target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=1)
    message(STATUS "Found NUMA library: ${NUMA_LIBRARY}")
  else()
    target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
    message(WARNING "NUMA library not found")
  endif()
else()
  message(STATUS "CPU affinity features disabled")
  target_compile_definitions(${FLOX} PUBLIC FLOX_CPU_AFFINITY_ENABLED=0)
  target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
endif()

option(FLOX_ENABLE_LZ4 "Enable LZ4 compression for binary logs" ON)
if(FLOX_ENABLE_LZ4)
  # Try find_package first (works with vcpkg on Windows)
  find_package(lz4 CONFIG QUIET)
  if(lz4_FOUND)
    target_link_libraries(${FLOX} PUBLIC lz4::lz4)
    message(STATUS "LZ4 found via find_package")
  else()
    # Try find_library (works with brew on macOS)
    find_library(LZ4_LIBRARY NAMES lz4)
    find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
    if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
      target_include_directories(${FLOX} PUBLIC ${LZ4_INCLUDE_DIR})
      target_link_libraries(${FLOX} PUBLIC ${LZ4_LIBRARY})
      message(STATUS "LZ4 found via find_library: ${LZ4_LIBRARY}")
    else()
      # Try pkg-config (Linux with liblz4-devel installed)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 QUIET liblz4)
      endif()
      if(LZ4_FOUND)
        target_include_directories(${FLOX} PUBLIC ${LZ4_INCLUDE_DIRS})
        target_link_libraries(${FLOX} PUBLIC ${LZ4_LINK_LIBRARIES})
        message(STATUS "LZ4 found via pkg-config")
      else()
        # Last resort: vendor lz4 source and compile it directly into flox.
        # We avoid lz4's own cmake build because lz4_static lands in lz4's own
        # export set, conflicting with install(EXPORT floxTargets) below
        # (a target can belong to only one export set in CMake). Compiling
        # lz4.c into ${FLOX} sidesteps the issue and keeps lz4 symbols
        # available to compression.h consumers without a separate target.
        include(FetchContent)
        FetchContent_Declare(
          lz4_src
          GIT_REPOSITORY https://github.com/lz4/lz4.git
          GIT_TAG v1.10.0
          GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(lz4_src)
        if(NOT lz4_src_POPULATED)
          FetchContent_Populate(lz4_src)
        endif()
        target_sources(${FLOX} PRIVATE ${lz4_src_SOURCE_DIR}/lib/lz4.c)
        target_include_directories(${FLOX} PUBLIC
          $<BUILD_INTERFACE:${lz4_src_SOURCE_DIR}/lib>
        )
        # Install vendored lz4 header so installed flox consumers can include it.
        install(FILES ${lz4_src_SOURCE_DIR}/lib/lz4.h DESTINATION include)
        message(STATUS "LZ4 vendored from sources (compiled into flox)")
      endif()
    endif()
  endif()
  target_compile_definitions(${FLOX} PUBLIC FLOX_LZ4_ENABLED=1)
  message(STATUS "LZ4 compression enabled")
else()
  target_compile_definitions(${FLOX} PUBLIC FLOX_LZ4_ENABLED=0)
  message(STATUS "LZ4 compression disabled")
endif()

option(FLOX_ENABLE_TRACY "Enable Tracy profiler" OFF)
if (FLOX_ENABLE_TRACY)
  include(FetchContent)
  FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.10
  )

  FetchContent_MakeAvailable(tracy)

  target_link_libraries(${FLOX} PUBLIC Tracy::TracyClient)
  target_compile_definitions(${FLOX} PUBLIC FLOX_ENABLE_TRACY=1)
endif()

# ──────────────────────────────────────────────────────────────────────
# Build artefacts (FLOX_BUILD_*)
#
# Naming convention: FLOX_BUILD_* gates optional build outputs (binding
# wheels, demo binary, tools, tests, benchmarks, connectors); FLOX_ENABLE_*
# gates capabilities of the core library (backtest module, LZ4, Tracy,
# CPU affinity).
#
# The eight FLOX_BUILD_* options below were renamed from FLOX_ENABLE_*
# in W9-T002. The deprecated names continue to work as aliases for one
# release cycle — passing -DFLOX_ENABLE_PYTHON=ON still flips
# FLOX_BUILD_PYTHON, with a warning. See docs/build/feature-flags.md.
# ──────────────────────────────────────────────────────────────────────

# Deprecation aliases: copy each old FLOX_ENABLE_<X> to FLOX_BUILD_<X>
# when the new form was not set explicitly. Emits a one-line warning
# per legacy use so downstream caches notice and migrate.
macro(_flox_deprecate_enable_to_build name)
  if(DEFINED FLOX_ENABLE_${name} AND NOT DEFINED FLOX_BUILD_${name})
    message(WARNING
      "FLOX_ENABLE_${name} is deprecated; use FLOX_BUILD_${name}. "
      "The legacy name will be removed in a future release.")
    set(FLOX_BUILD_${name} ${FLOX_ENABLE_${name}})
  endif()
endmacro()

foreach(_flox_renamed BENCHMARKS TESTS DEMO TOOLS PYTHON CAPI CODON QUICKJS)
  _flox_deprecate_enable_to_build(${_flox_renamed})
endforeach()

option(FLOX_BUILD_BENCHMARKS "Build benchmarks" OFF)
if (FLOX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()


option(FLOX_BUILD_TESTS "Build tests" OFF)
if (FLOX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(FLOX_BUILD_DEMO "Build demo application" OFF)
if (FLOX_BUILD_DEMO)
  add_subdirectory(demo)
endif()

option(FLOX_BUILD_TOOLS "Build command-line tools" OFF)
if (FLOX_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

option(FLOX_BUILD_PYTHON "Build Python bindings" OFF)
if (FLOX_BUILD_PYTHON)
  add_subdirectory(python)
endif()

# FLOX_BUILD_NODE is an explicit flag for parity with the other
# binding artefacts. The Node addon is built out-of-tree by
# `npm run build` — this flag exists for documentation completeness
# and so the CI matrix can reference it uniformly. CMake itself does
# not invoke npm.
option(FLOX_BUILD_NODE "Build Node.js bindings (out-of-tree via npm)" OFF)

option(FLOX_BUILD_CAPI "Build C API shared library" OFF)
if (FLOX_BUILD_CAPI)
  # Static lib must be PIC to link into the shared CAPI lib
  set_target_properties(${FLOX} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # The C API exposes the full FLOX surface — including the
  # backtest module — so the shared library has to link against
  # backtest_*.cpp. Auto-enable BACKTEST when it isn't already on.
  if(NOT FLOX_ENABLE_BACKTEST)
    message(STATUS "FLOX_BUILD_CAPI=ON implies FLOX_ENABLE_BACKTEST=ON; enabling.")
    set(FLOX_ENABLE_BACKTEST ON CACHE BOOL "Enable backtest module" FORCE)
    file(GLOB BACKTEST_SRC_FILES src/backtest/*.cpp)
    target_sources(${FLOX} PRIVATE ${BACKTEST_SRC_FILES})
  endif()
  add_subdirectory(src/capi)
endif()

option(FLOX_BUILD_CODON "Build Codon strategy support" OFF)
if (FLOX_BUILD_CODON)
  if (NOT FLOX_BUILD_CAPI)
    message(FATAL_ERROR "FLOX_BUILD_CODON requires FLOX_BUILD_CAPI=ON")
  endif()
  add_subdirectory(codon)
endif()

option(FLOX_BUILD_QUICKJS "Build QuickJS JavaScript strategy support" OFF)
if (FLOX_BUILD_QUICKJS)
  if (NOT FLOX_BUILD_CAPI)
    message(FATAL_ERROR "FLOX_BUILD_QUICKJS requires FLOX_BUILD_CAPI=ON")
  endif()
  add_subdirectory(src/quickjs)
endif()

# Native exchange connectors (Bybit, Bitget, Hyperliquid, Polymarket).
# OFF by default so a backtest-only or research build doesn't pay the
# dependency cost (OpenSSL/CURL/zlib + ixwebsocket/simdjson via
# FetchContent). See connectors/CMakeLists.txt for the per-connector
# wiring.
option(FLOX_BUILD_CONNECTORS "Build native exchange connectors module" OFF)

# Subset of connectors to build when FLOX_BUILD_CONNECTORS=ON. Empty
# string (the default) means "all venues currently in connectors/src/".
# Validation lives in connectors/CMakeLists.txt; a configure-time
# error fires when an unknown name is requested.
set(FLOX_CONNECTORS "" CACHE STRING
    "Semicolon-separated list of venues to build under \
connectors/src/<name>/; empty = all available")

if (FLOX_BUILD_CONNECTORS)
  add_subdirectory(connectors)
endif()

target_include_directories(flox PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

install(TARGETS flox
  EXPORT floxTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

install(EXPORT floxTargets
  FILE floxTargets.cmake
  NAMESPACE flox::
  DESTINATION lib/cmake/flox
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/floxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfig.cmake"
  INSTALL_DESTINATION lib/cmake/flox
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfigVersion.cmake"
  DESTINATION lib/cmake/flox
)
