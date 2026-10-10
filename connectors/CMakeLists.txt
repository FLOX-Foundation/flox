# connectors/CMakeLists.txt
#
# Built by the parent lrvx project when -DLRVX_BUILD_CONNECTORS=ON.
# Pulls ixwebsocket and simdjson via FetchContent so the consuming
# repo doesn't have to manage submodules.
#
# The static library is `lrvx-connectors` (alias `lrvx::connectors`).
# Its public headers live under include/lrvx-connectors/ and are
# included as `#include "lrvx-connectors/..."`.

include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(ZLIB    REQUIRED)
find_package(CURL    REQUIRED)

# ── Network deps via FetchContent ────────────────────────────────────
#
# Pinned to the SHAs the standalone lrvx-connectors repo used as
# submodules. Refresh together if either upstream changes shape.

set(USE_TLS      ON  CACHE BOOL "Enable TLS support in ixwebsocket" FORCE)
set(USE_OPEN_SSL ON  CACHE BOOL "Use OpenSSL backend"               FORCE)
set(USE_ZLIB     ON  CACHE BOOL "Use zlib in ixwebsocket"           FORCE)

FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        a89718b50c54dfeaa3e1386b94b189864ef4acff
)
FetchContent_MakeAvailable(ixwebsocket)

FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        0c0ce1bd48baa0677dc7c0945ea7cd1e8b52b297
)
FetchContent_MakeAvailable(simdjson)

# ── Polymarket Order Executor (Rust FFI, optional) ──────────────────

option(LRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR
       "Enable Polymarket order executor (requires Rust toolchain)" ON)
set(POLYMARKET_FFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/polymarket/ffi")

find_program(CARGO_EXECUTABLE cargo)
if(LRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR AND CARGO_EXECUTABLE)
  set(POLYMARKET_FFI_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
  set(POLYMARKET_FFI_LIB
      "${POLYMARKET_FFI_TARGET_DIR}/release/libpolymarket_executor.a")

  add_custom_command(
    OUTPUT ${POLYMARKET_FFI_LIB}
    COMMAND ${CMAKE_COMMAND} -E env
            CARGO_TARGET_DIR=${POLYMARKET_FFI_TARGET_DIR}
            ${CARGO_EXECUTABLE} build --release
    WORKING_DIRECTORY ${POLYMARKET_FFI_DIR}
    COMMENT "Building Polymarket FFI executor..."
    VERBATIM
  )
  add_custom_target(polymarket_ffi_executor DEPENDS ${POLYMARKET_FFI_LIB})

  set(LRVX_POLYMARKET_EXECUTOR_AVAILABLE TRUE)
  message(STATUS "Polymarket Order Executor: ENABLED (Rust)")
elseif(LRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR)
  message(WARNING
    "cargo not found — Polymarket order executor will be disabled. "
    "Install rustup or pass -DLRVX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF "
    "to silence this warning.")
  set(LRVX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
else()
  set(LRVX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
endif()

# ── Library ──────────────────────────────────────────────────────────

# Available venues are inferred from the immediate subdirectories of
# `src/` that contain at least one .cpp. The `LRVX_CONNECTORS` cache
# variable (defined by the parent) optionally narrows the build to a
# subset; empty value means "all".
file(GLOB _lrvx_connectors_venue_dirs
     LIST_DIRECTORIES true CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*)
set(_lrvx_connectors_available "")
foreach(d IN LISTS _lrvx_connectors_venue_dirs)
  if(IS_DIRECTORY "${d}")
    get_filename_component(_name "${d}" NAME)
    list(APPEND _lrvx_connectors_available "${_name}")
  endif()
endforeach()
list(SORT _lrvx_connectors_available)

if(LRVX_CONNECTORS STREQUAL "")
  set(_lrvx_connectors_selected "${_lrvx_connectors_available}")
else()
  set(_lrvx_connectors_selected "${LRVX_CONNECTORS}")
  foreach(_v IN LISTS _lrvx_connectors_selected)
    if(NOT _v IN_LIST _lrvx_connectors_available)
      message(FATAL_ERROR
        "LRVX_CONNECTORS includes unknown venue '${_v}'. "
        "Available: ${_lrvx_connectors_available}.")
    endif()
  endforeach()
endif()

set(LRVX_CONNECTORS_SRC "")
foreach(_venue IN LISTS _lrvx_connectors_selected)
  file(GLOB_RECURSE _venue_src CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/src/${_venue}/*.cpp)
  list(APPEND LRVX_CONNECTORS_SRC ${_venue_src})
endforeach()
# `execution/` and `net/` are shared infrastructure; always include them
# unless explicitly excluded as a venue (they're not exchange names but
# share the src/ directory layout).
foreach(_shared execution net)
  if(NOT _shared IN_LIST _lrvx_connectors_selected
     AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/${_shared}")
    file(GLOB_RECURSE _shared_src CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/${_shared}/*.cpp)
    list(APPEND LRVX_CONNECTORS_SRC ${_shared_src})
  endif()
endforeach()

if(NOT LRVX_POLYMARKET_EXECUTOR_AVAILABLE)
  list(FILTER LRVX_CONNECTORS_SRC
       EXCLUDE REGEX ".*polymarket_order_executor\\.cpp$")
endif()

message(STATUS "Connectors: building ${_lrvx_connectors_selected}")

add_library(lrvx-connectors STATIC ${LRVX_CONNECTORS_SRC})
add_library(lrvx::connectors ALIAS lrvx-connectors)

if(WIN32)
  target_compile_definitions(lrvx-connectors PRIVATE NOMINMAX)
endif()

target_include_directories(lrvx-connectors
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
)

if(LRVX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_compile_definitions(lrvx-connectors
    PUBLIC LRVX_POLYMARKET_ORDER_EXECUTOR_ENABLED=1)
  add_dependencies(lrvx-connectors polymarket_ffi_executor)
endif()

target_link_libraries(lrvx-connectors
  PUBLIC  lrvx::lrvx
          simdjson::simdjson
          ixwebsocket
  PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB CURL::libcurl
          $<$<PLATFORM_ID:Windows>:ws2_32>
          $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>
)

if(LRVX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_link_libraries(lrvx-connectors PUBLIC ${POLYMARKET_FFI_LIB} dl)
endif()

# ── Install ──────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS lrvx-connectors
        EXPORT  lrvx-connectorsTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT lrvx-connectorsTargets
        NAMESPACE lrvx::
        FILE lrvx-connectors-config.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lrvx-connectors)

# ── Tests ────────────────────────────────────────────────────────────
#
# Two kinds live in tests/: offline protocol unit tests (unit_test_*,
# no network -- always built and registered with ctest when the parent
# builds tests) and the integration suite (integration_test_*, hits
# live exchange endpoints; built only with
# LRVX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON and registered with ctest
# only under LRVX_RUN_CONNECTOR_INTEGRATION_TESTS=ON).

option(LRVX_BUILD_CONNECTOR_INTEGRATION_TESTS
       "Build connector integration tests (require network access)" OFF)

if(LRVX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
