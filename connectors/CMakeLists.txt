# connectors/CMakeLists.txt
#
# Built by the parent flox project when -DFLOX_BUILD_CONNECTORS=ON.
# Pulls ixwebsocket and simdjson via FetchContent so the consuming
# repo doesn't have to manage submodules.
#
# The static library still uses the historical name `flox-connectors`
# (with `flox::connectors` alias) so that source files imported with
# the original include layout (`#include "flox-connectors/..."`)
# resolve unchanged.

include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(ZLIB    REQUIRED)
find_package(CURL    REQUIRED)

# ── Network deps via FetchContent ────────────────────────────────────
#
# Pinned to the SHAs the standalone flox-connectors repo used as
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

option(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR
       "Enable Polymarket order executor (requires Rust toolchain)" ON)
set(POLYMARKET_FFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/polymarket/ffi")

find_program(CARGO_EXECUTABLE cargo)
if(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR AND CARGO_EXECUTABLE)
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

  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE TRUE)
  message(STATUS "Polymarket Order Executor: ENABLED (Rust)")
elseif(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR)
  message(WARNING
    "cargo not found — Polymarket order executor will be disabled. "
    "Install rustup or pass -DFLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR=OFF "
    "to silence this warning.")
  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
else()
  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
endif()

# ── Library ──────────────────────────────────────────────────────────

# Available venues are inferred from the immediate subdirectories of
# `src/` that contain at least one .cpp. The `FLOX_CONNECTORS` cache
# variable (defined by the parent) optionally narrows the build to a
# subset; empty value means "all".
file(GLOB _flox_connectors_venue_dirs
     LIST_DIRECTORIES true CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*)
set(_flox_connectors_available "")
foreach(d IN LISTS _flox_connectors_venue_dirs)
  if(IS_DIRECTORY "${d}")
    get_filename_component(_name "${d}" NAME)
    list(APPEND _flox_connectors_available "${_name}")
  endif()
endforeach()
list(SORT _flox_connectors_available)

if(FLOX_CONNECTORS STREQUAL "")
  set(_flox_connectors_selected "${_flox_connectors_available}")
else()
  set(_flox_connectors_selected "${FLOX_CONNECTORS}")
  foreach(_v IN LISTS _flox_connectors_selected)
    if(NOT _v IN_LIST _flox_connectors_available)
      message(FATAL_ERROR
        "FLOX_CONNECTORS includes unknown venue '${_v}'. "
        "Available: ${_flox_connectors_available}.")
    endif()
  endforeach()
endif()

set(FLOX_CONNECTORS_SRC "")
foreach(_venue IN LISTS _flox_connectors_selected)
  file(GLOB_RECURSE _venue_src CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/src/${_venue}/*.cpp)
  list(APPEND FLOX_CONNECTORS_SRC ${_venue_src})
endforeach()
# `execution/` and `net/` are shared infrastructure; always include them
# unless explicitly excluded as a venue (they're not exchange names but
# share the src/ directory layout).
foreach(_shared execution net)
  if(NOT _shared IN_LIST _flox_connectors_selected
     AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/${_shared}")
    file(GLOB_RECURSE _shared_src CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/${_shared}/*.cpp)
    list(APPEND FLOX_CONNECTORS_SRC ${_shared_src})
  endif()
endforeach()

if(NOT FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  list(FILTER FLOX_CONNECTORS_SRC
       EXCLUDE REGEX ".*polymarket_order_executor\\.cpp$")
endif()

message(STATUS "Connectors: building ${_flox_connectors_selected}")

add_library(flox-connectors STATIC ${FLOX_CONNECTORS_SRC})
add_library(flox::connectors ALIAS flox-connectors)

if(WIN32)
  target_compile_definitions(flox-connectors PRIVATE NOMINMAX)
endif()

target_include_directories(flox-connectors
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
)

if(FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_compile_definitions(flox-connectors
    PUBLIC FLOX_POLYMARKET_ORDER_EXECUTOR_ENABLED=1)
  add_dependencies(flox-connectors polymarket_ffi_executor)
endif()

target_link_libraries(flox-connectors
  PUBLIC  flox::flox
          simdjson::simdjson
          ixwebsocket
  PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB CURL::libcurl
          $<$<PLATFORM_ID:Windows>:ws2_32>
          $<$<NOT:$<PLATFORM_ID:Windows>>:pthread>
)

if(FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_link_libraries(flox-connectors PUBLIC ${POLYMARKET_FFI_LIB} dl)
endif()

# ── Install ──────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS flox-connectors
        EXPORT  flox-connectorsTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT flox-connectorsTargets
        NAMESPACE flox::
        FILE flox-connectors-config.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flox-connectors)

# ── Tests ────────────────────────────────────────────────────────────
#
# Built only when the parent enables tests AND the user explicitly
# opts in to the integration suite (it hits live exchange endpoints
# and needs network + credentials). Without
# FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON the test binaries are not
# generated and `ctest` still runs only the core suite.

option(FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS
       "Build connector integration tests (require network access)" OFF)

if(FLOX_BUILD_TESTS AND FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS)
  add_subdirectory(tests)
endif()
