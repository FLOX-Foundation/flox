cmake_minimum_required(VERSION 3.20)
project(flox-connectors LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_DEV_SETUP "Install pre-commit hook" OFF)

if(ENABLE_DEV_SETUP)
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

find_package(OpenSSL  REQUIRED)
find_package(ZLIB     REQUIRED)
find_package(CURL     REQUIRED)

set(USE_TLS      ON  CACHE BOOL "Enable TLS support in ixwebsocket" FORCE)
set(USE_OPEN_SSL ON  CACHE BOOL "Use OpenSSL backend"               FORCE)
set(USE_ZLIB     ON  CACHE BOOL "Use zlib in ixwebsocket"           FORCE)

add_subdirectory(external/flox)
add_subdirectory(external/simdjson)
add_subdirectory(external/ixwebsocket)

# Polymarket Order Executor (Rust FFI)
option(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR "Enable Polymarket order executor" ON)
set(POLYMARKET_FFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/polymarket/ffi")

# Build Rust library if cargo is available
find_program(CARGO_EXECUTABLE cargo)
if(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR AND CARGO_EXECUTABLE)
  # Build into CMAKE_BINARY_DIR to keep source clean
  set(POLYMARKET_FFI_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
  set(POLYMARKET_FFI_LIB "${POLYMARKET_FFI_TARGET_DIR}/release/libpolymarket_executor.a")

  # Custom command to build Rust library
  add_custom_command(
    OUTPUT ${POLYMARKET_FFI_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${POLYMARKET_FFI_TARGET_DIR}
            ${CARGO_EXECUTABLE} build --release
    WORKING_DIRECTORY ${POLYMARKET_FFI_DIR}
    COMMENT "Building Polymarket FFI executor..."
    VERBATIM
  )

  add_custom_target(polymarket_ffi_executor DEPENDS ${POLYMARKET_FFI_LIB})

  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE TRUE)
  message(STATUS "Polymarket Order Executor: ENABLED (Rust)")
elseif(FLOX_ENABLE_POLYMARKET_ORDER_EXECUTOR)
  message(WARNING "cargo not found - Polymarket order executor will be disabled")
  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
else()
  set(FLOX_POLYMARKET_EXECUTOR_AVAILABLE FALSE)
endif()

file(GLOB_RECURSE FLOX_CONNECTORS_SRC CONFIGURE_DEPENDS src/*.cpp)

# Exclude order executor if Rust not available
if(NOT FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  list(FILTER FLOX_CONNECTORS_SRC EXCLUDE REGEX ".*polymarket_order_executor\\.cpp$")
endif()

add_library(flox-connectors STATIC ${FLOX_CONNECTORS_SRC})

if(WIN32)
  target_compile_definitions(flox-connectors PRIVATE NOMINMAX)
endif()

target_include_directories(flox-connectors
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
)

# Add Polymarket FFI link if available
if(FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_compile_definitions(flox-connectors PUBLIC FLOX_POLYMARKET_ORDER_EXECUTOR_ENABLED=1)
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

# Link Polymarket FFI library if available
if(FLOX_POLYMARKET_EXECUTOR_AVAILABLE)
  target_link_libraries(flox-connectors PUBLIC ${POLYMARKET_FFI_LIB} dl)
endif()

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

option(FLOX_CONNECTORS_BUILD_TESTS "Build connectors tests" OFF)
if(FLOX_CONNECTORS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(external/googletest)
    add_subdirectory(tests)
endif()
