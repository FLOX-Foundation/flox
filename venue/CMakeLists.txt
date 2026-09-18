# ── flox venue module ────────────────────────────────────────────────
# The venue side of flox: build a market rather than trade on one. Kept as an
# optional module (mirroring `connectors/`) so the core library stays free of
# its weight, and so the server perimeter can carry its own dependencies
# without pushing them onto every flox user.
#
# Namespace: flox::venue.  Include prefix: <flox-venue/...>.

# The module splits in two along a line the platforms draw for us. The
# engine -- matching, clearing, risk, journal, recovery -- needs a 128-bit
# integer and nothing else from the system. The perimeter -- gateways,
# sessions, market-data distribution, the control and metrics servers --
# needs POSIX sockets, and there is no Winsock port yet. Building the first
# without the second is what lets a venue exist on a platform the second
# does not reach.
if(NOT DEFINED FLOX_VENUE_PERIMETER)
  if(WIN32)
    set(FLOX_VENUE_PERIMETER OFF)
    message(STATUS "FLOX_VENUE_PERIMETER: no POSIX sockets here; engine only.")
  else()
    set(FLOX_VENUE_PERIMETER ON)
  endif()
endif()
option(FLOX_VENUE_PERIMETER "Build the venue's network perimeter (needs POSIX sockets)" ${FLOX_VENUE_PERIMETER})

# simdjson is the REST codec's parser and the REST codec is perimeter.
if(FLOX_VENUE_PERIMETER)
include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        0c0ce1bd48baa0677dc7c0945ea7cd1e8b52b297
)
FetchContent_MakeAvailable(simdjson)
endif()

file(GLOB_RECURSE FLOX_VENUE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(flox-venue STATIC ${FLOX_VENUE_SRC})
add_library(flox::venue ALIAS flox-venue)

target_include_directories(flox-venue
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# 128-bit arithmetic needs runtime helpers -- __divti3, __floattidf, __fixdfti
# -- and where they come from is a property of the toolchain, not of the
# language. GCC and Clang on Unix link libgcc or compiler-rt for you. clang-cl
# targeting the MSVC ABI does not: the Microsoft link line has no such default,
# and the omission surfaces as undefined symbols at the very end of a build.
#
# So the library is found now, at configure time, and a configure that cannot
# find it says so here instead of letting the link fail eight minutes later.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(FLOX_CLANG_BUILTINS "")
  # Ask the compiler first; it knows where its own runtime lives.
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" --rtlib=compiler-rt -print-libgcc-file-name
    OUTPUT_VARIABLE _rt OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _rt_rc)
  if(_rt_rc EQUAL 0 AND EXISTS "${_rt}")
    set(FLOX_CLANG_BUILTINS "${_rt}")
  else()
    # Otherwise look where clang keeps it, next to the driver.
    get_filename_component(_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_root "${_bin}" DIRECTORY)
    file(GLOB_RECURSE _found "${_root}/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
    if(_found)
      list(GET _found 0 FLOX_CLANG_BUILTINS)
    endif()
  endif()
  if(NOT FLOX_CLANG_BUILTINS)
    message(FATAL_ERROR
      "FLOX_BUILD_VENUE=ON with clang-cl needs clang_rt.builtins for 128-bit "
      "arithmetic (__divti3 and friends) and it was not found near "
      "${CMAKE_CXX_COMPILER}. Install the LLVM runtime, or configure with "
      "-DFLOX_BUILD_VENUE=OFF.")
  endif()
  message(STATUS "FLOX_BUILD_VENUE: 128-bit helpers from ${FLOX_CLANG_BUILTINS}")
  target_link_libraries(flox-venue PUBLIC "${FLOX_CLANG_BUILTINS}")
endif()

if(FLOX_VENUE_PERIMETER)
  target_link_libraries(flox-venue PUBLIC ${FLOX} simdjson::simdjson)
  target_compile_definitions(flox-venue PUBLIC FLOX_VENUE_PERIMETER=1)
else()
  target_link_libraries(flox-venue PUBLIC ${FLOX})
endif()
target_compile_features(flox-venue PUBLIC cxx_std_23)

# ── benchmarks ───────────────────────────────────────────────────────
if(FLOX_BUILD_BENCHMARKS)
  add_executable(bench_venue_book ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_book.cpp)
  target_link_libraries(bench_venue_book PRIVATE flox-venue)
endif()

# ── Install ──────────────────────────────────────────────────────────
# Same posture as connectors/: the module is consumable via find_package so a
# downstream project can link flox::venue without vendoring this tree.

include(GNUInstallDirs)

install(TARGETS flox-venue
        EXPORT  flox-venueTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT flox-venueTargets
        NAMESPACE flox::
        FILE flox-venue-config.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flox-venue)

# ── tests ────────────────────────────────────────────────────────────
if(FLOX_BUILD_TESTS)
  if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()
  find_package(Threads)
  file(GLOB FLOX_VENUE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  if(NOT FLOX_VENUE_PERIMETER)
    # Derived from what each test includes, not from a list someone keeps in
    # step. The first version of this was a list of names written by hand and
    # it was wrong on the first build: four tests that needed the perimeter
    # were not on it, and two that were on it did not exist.
    set(FLOX_VENUE_PERIMETER_HEADERS
        socket_acceptor.h tcp_gateway.h ws_gateway.h tls_gateway.h udp_multicast.h
        session.h session_registry.h cancel_on_disconnect.h fix_session.h
        md_distribution.h md_recovery.h control_server.h control_api.h
        metrics_server.h rest_json.h)
    set(FLOX_VENUE_ENGINE_TESTS "")
    set(FLOX_VENUE_DROPPED "")
    foreach(t ${FLOX_VENUE_TESTS})
      file(STRINGS ${t} _includes REGEX "^#include \"flox-venue/")
      set(_needs_perimeter FALSE)
      foreach(h ${FLOX_VENUE_PERIMETER_HEADERS})
        foreach(line ${_includes})
          if(line MATCHES "flox-venue/${h}\"")
            set(_needs_perimeter TRUE)
          endif()
        endforeach()
      endforeach()
      if(_needs_perimeter)
        get_filename_component(_n ${t} NAME_WE)
        list(APPEND FLOX_VENUE_DROPPED ${_n})
      else()
        list(APPEND FLOX_VENUE_ENGINE_TESTS ${t})
      endif()
    endforeach()
    set(FLOX_VENUE_TESTS ${FLOX_VENUE_ENGINE_TESTS})
    list(LENGTH FLOX_VENUE_DROPPED _n_dropped)
    message(STATUS "FLOX_VENUE_PERIMETER=OFF: ${_n_dropped} tests need the perimeter: ${FLOX_VENUE_DROPPED}")
  endif()
  # The TLS gateway needs OpenSSL; the rest of the module does not. Keep the
  # heavy dependency optional (same posture as connectors/) so a plain build of
  # the venue stays dependency-free.
  find_package(OpenSSL)
  if(NOT OpenSSL_FOUND)
    list(REMOVE_ITEM FLOX_VENUE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_venue_tls.cpp)
    message(STATUS "OpenSSL not found -- skipping venue TLS gateway test")
  endif()
  foreach(test_src ${FLOX_VENUE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name}
      PRIVATE flox-venue GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
    if(TARGET Threads::Threads)
      target_link_libraries(${test_name} PRIVATE Threads::Threads)
    endif()
    if(test_name STREQUAL "test_venue_tls" AND OpenSSL_FOUND)
      target_link_libraries(${test_name} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
