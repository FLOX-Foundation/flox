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
    # Not "no POSIX sockets" any more: the perimeter goes through
    # flox/net/socket.h and compiles here. What is still unfinished is the
    # rest of the perimeter's surroundings on this platform -- simdjson and
    # OpenSSL under clang-cl, and one lifecycle test that interposes close(2)
    # through dlsym. Off by default until those are answered; the option is
    # there for whoever wants to try.
    message(STATUS "FLOX_VENUE_PERIMETER: off by default on Windows; engine only.")
  else()
    set(FLOX_VENUE_PERIMETER ON)
  endif()
endif()
option(FLOX_VENUE_PERIMETER "Build the venue's network perimeter" ${FLOX_VENUE_PERIMETER})

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

# The TLS gateway's header is installed and includes <openssl/...>, so the
# target has to say so. It did not: OpenSSL was found inside the tests block
# and linked to one test, which is why nothing in this tree noticed -- a
# consumer who took flox::venue through find_package and included
# tls_gateway.h got undefined SSL_* symbols and no indication why.
#
# Declared here instead, PUBLIC, because it is the header that needs it and
# the header is part of the installed surface. Without OpenSSL the gateway
# cannot be built at all, so the header is excluded from the install rather
# than shipped broken.
if(FLOX_VENUE_PERIMETER)
  find_package(OpenSSL)
  if(OpenSSL_FOUND)
    set(FLOX_VENUE_TLS ON)
  else()
    set(FLOX_VENUE_TLS OFF)
    message(STATUS "OpenSSL not found: the venue's TLS gateway is not built or installed")
  endif()
else()
  set(FLOX_VENUE_TLS OFF)
endif()

if(FLOX_VENUE_PERIMETER)
  target_link_libraries(flox-venue PUBLIC ${FLOX} simdjson::simdjson)
  target_compile_definitions(flox-venue PUBLIC FLOX_VENUE_PERIMETER=1)
  if(FLOX_VENUE_TLS)
    target_link_libraries(flox-venue PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(flox-venue PUBLIC FLOX_VENUE_TLS=1)
  endif()
else()
  target_link_libraries(flox-venue PUBLIC ${FLOX})
endif()
target_compile_features(flox-venue PUBLIC cxx_std_23)

# ── benchmarks ───────────────────────────────────────────────────────
if(FLOX_BUILD_BENCHMARKS)
  add_executable(bench_venue_book ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_book.cpp)
  target_link_libraries(bench_venue_book PRIVATE flox-venue)
  add_executable(bench_venue_generated_flow
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_generated_flow.cpp)
  target_link_libraries(bench_venue_generated_flow PRIVATE flox-venue)
  # Sockets: perimeter only.
  if(FLOX_VENUE_PERIMETER)
    add_executable(bench_venue_md_decoder_seam
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_md_decoder_seam.cpp)
    target_link_libraries(bench_venue_md_decoder_seam PRIVATE flox-venue)
  endif()
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

if(FLOX_VENUE_TLS)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  # A header whose dependency is not there is worse installed than absent: the
  # consumer discovers it at link time with no hint of the cause.
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          PATTERN "tls_gateway.h" EXCLUDE)
endif()

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
  # Derived the same way the perimeter split is: a test that includes the TLS
  # channel needs the flag that builds it. Without the flag the header refuses
  # to compile, on purpose, so the test is dropped rather than broken.
  if(NOT FLOX_FIX_TLS)
    foreach(t ${FLOX_VENUE_TESTS})
      file(STRINGS ${t} _tlsinc REGEX "fix_tls_channel.h")
      if(_tlsinc)
        list(REMOVE_ITEM FLOX_VENUE_TESTS ${t})
        get_filename_component(_n ${t} NAME)
        message(STATUS "FLOX_FIX_TLS is off: skipping ${_n}")
      endif()
    endforeach()
  endif()
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
  # The dependency is declared on the target now (see above), so a test that
  # uses the gateway needs nothing special -- it comes through flox-venue.
  if(NOT FLOX_VENUE_TLS)
    list(REMOVE_ITEM FLOX_VENUE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_venue_tls.cpp)
    message(STATUS "OpenSSL not found -- skipping venue TLS gateway test")
  endif()
  # A shard that writes a journal and then abandons the process. Used by the
  # process-death test on every platform: Windows has no fork, and a
  # Windows-only mechanism would be exercised only where nobody runs it by
  # hand. Excluded from the test list -- it has a main(), not a TEST().
  list(REMOVE_ITEM FLOX_VENUE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/journal_writer_main.cpp)
  add_executable(venue_journal_writer ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/journal_writer_main.cpp)
  target_link_libraries(venue_journal_writer PRIVATE flox-venue)
  target_include_directories(venue_journal_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  foreach(test_src ${FLOX_VENUE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name}
      PRIVATE flox-venue GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
    if(TARGET Threads::Threads)
      target_link_libraries(${test_name} PRIVATE Threads::Threads)
    endif()
    if(test_name STREQUAL "test_venue_recovery")
      # The helper's path rather than a guess at the build layout: a test that
      # looked for a sibling binary by name would find the wrong one, or none,
      # depending on the generator.
      target_compile_definitions(${test_name}
        PRIVATE FLOX_JOURNAL_WRITER="$<TARGET_FILE:venue_journal_writer>")
      add_dependencies(${test_name} venue_journal_writer)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
