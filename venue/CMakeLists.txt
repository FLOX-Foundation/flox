# ── lrvx venue module ────────────────────────────────────────────────
# The venue side of lrvx: build a market rather than trade on one. Kept as an
# optional module (mirroring `connectors/`) so the core library stays free of
# its weight, and so the server perimeter can carry its own dependencies
# without pushing them onto every lrvx user.
#
# Namespace: lrvx::venue.  Include prefix: <lrvx-venue/...>.

# The module splits in two along a line the platforms draw for us. The
# engine -- matching, clearing, risk, journal, recovery -- needs a 128-bit
# integer and nothing else from the system. The perimeter -- gateways,
# sessions, market-data distribution, the control and metrics servers --
# needs POSIX sockets, and there is no Winsock port yet. Building the first
# without the second is what lets a venue exist on a platform the second
# does not reach.
if(NOT DEFINED LRVX_VENUE_PERIMETER)
  if(WIN32)
    set(LRVX_VENUE_PERIMETER OFF)
    # Not "no POSIX sockets" any more: the perimeter goes through
    # lrvx/net/socket.h and compiles here. What is still unfinished is the
    # rest of the perimeter's surroundings on this platform -- simdjson and
    # OpenSSL under clang-cl, and one lifecycle test that interposes close(2)
    # through dlsym. Off by default until those are answered; the option is
    # there for whoever wants to try.
    message(STATUS "LRVX_VENUE_PERIMETER: off by default on Windows; engine only.")
  else()
    set(LRVX_VENUE_PERIMETER ON)
  endif()
endif()
option(LRVX_VENUE_PERIMETER "Build the venue's network perimeter" ${LRVX_VENUE_PERIMETER})

# simdjson is the REST codec's parser and the REST codec is perimeter.
if(LRVX_VENUE_PERIMETER)
include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        0c0ce1bd48baa0677dc7c0945ea7cd1e8b52b297
)
FetchContent_MakeAvailable(simdjson)
endif()

file(GLOB_RECURSE LRVX_VENUE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(lrvx-venue STATIC ${LRVX_VENUE_SRC})
add_library(lrvx::venue ALIAS lrvx-venue)

target_include_directories(lrvx-venue
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# The TLS gateway's header is installed and includes <openssl/...>, so the
# target has to say so. It did not: OpenSSL was found inside the tests block
# and linked to one test, which is why nothing in this tree noticed -- a
# consumer who took lrvx::venue through find_package and included
# tls_gateway.h got undefined SSL_* symbols and no indication why.
#
# Declared here instead, PUBLIC, because it is the header that needs it and
# the header is part of the installed surface. Without OpenSSL the gateway
# cannot be built at all, so the header is excluded from the install rather
# than shipped broken.
if(LRVX_VENUE_PERIMETER)
  find_package(OpenSSL)
  if(OpenSSL_FOUND)
    set(LRVX_VENUE_TLS ON)
  else()
    set(LRVX_VENUE_TLS OFF)
    message(STATUS "OpenSSL not found: the venue's TLS gateway is not built or installed")
  endif()
else()
  set(LRVX_VENUE_TLS OFF)
endif()

if(LRVX_VENUE_PERIMETER)
  target_link_libraries(lrvx-venue PUBLIC ${LRVX} simdjson::simdjson)
  target_compile_definitions(lrvx-venue PUBLIC LRVX_VENUE_PERIMETER=1)
  if(LRVX_VENUE_TLS)
    target_link_libraries(lrvx-venue PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(lrvx-venue PUBLIC LRVX_VENUE_TLS=1)
  endif()
else()
  target_link_libraries(lrvx-venue PUBLIC ${LRVX})
endif()
target_compile_features(lrvx-venue PUBLIC cxx_std_23)

# ── benchmarks ───────────────────────────────────────────────────────
if(LRVX_BUILD_BENCHMARKS)
  add_executable(bench_venue_book ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_book.cpp)
  target_link_libraries(bench_venue_book PRIVATE lrvx-venue)
  add_executable(bench_venue_generated_flow
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_generated_flow.cpp)
  target_link_libraries(bench_venue_generated_flow PRIVATE lrvx-venue)
  add_executable(bench_venue_checkpoint_lane
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_checkpoint_lane.cpp)
  target_link_libraries(bench_venue_checkpoint_lane PRIVATE lrvx-venue)
  add_executable(bench_venue_shard_wake_set
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_shard_wake_set.cpp)
  target_link_libraries(bench_venue_shard_wake_set PRIVATE lrvx-venue)
  add_executable(bench_venue_quote_ladder
                 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_quote_ladder.cpp)
  target_link_libraries(bench_venue_quote_ladder PRIVATE lrvx-venue)
  # Sockets: perimeter only.
  if(LRVX_VENUE_PERIMETER)
    add_executable(bench_venue_md_decoder_seam
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench_md_decoder_seam.cpp)
    target_link_libraries(bench_venue_md_decoder_seam PRIVATE lrvx-venue)
  endif()
endif()

# ── Install ──────────────────────────────────────────────────────────
# Same posture as connectors/: the module is consumable via find_package so a
# downstream project can link lrvx::venue without vendoring this tree.

include(GNUInstallDirs)

install(TARGETS lrvx-venue
        EXPORT  lrvx-venueTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# In the lite profile the root CMakeLists installs the closure of
# venue/lite_surface.txt, this module's headers included, and installing the
# directory here on top of that would put back the 60-odd headers the profile
# exists to leave out.
if(LRVX_VENUE_LITE)
  message(STATUS "LRVX_VENUE_LITE: venue headers are installed as part of the lite closure")
elseif(LRVX_VENUE_TLS)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
  # A header whose dependency is not there is worse installed than absent: the
  # consumer discovers it at link time with no hint of the cause.
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
          PATTERN "tls_gateway.h" EXCLUDE)
endif()

install(EXPORT lrvx-venueTargets
        NAMESPACE lrvx::
        FILE lrvx-venue-config.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lrvx-venue)

# ── tests ────────────────────────────────────────────────────────────
if(LRVX_BUILD_TESTS)
  if(NOT TARGET GTest::gtest)
  find_package(GTest REQUIRED)
endif()
  find_package(Threads)
  file(GLOB LRVX_VENUE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  # Derived the same way the perimeter split is: a test that includes the TLS
  # channel needs the flag that builds it. Without the flag the header refuses
  # to compile, on purpose, so the test is dropped rather than broken.
  if(NOT LRVX_FIX_TLS)
    foreach(t ${LRVX_VENUE_TESTS})
      file(STRINGS ${t} _tlsinc REGEX "fix_tls_channel.h")
      if(_tlsinc)
        list(REMOVE_ITEM LRVX_VENUE_TESTS ${t})
        get_filename_component(_n ${t} NAME)
        message(STATUS "LRVX_FIX_TLS is off: skipping ${_n}")
      endif()
    endforeach()
  endif()
  if(NOT LRVX_VENUE_PERIMETER)
    # Derived from what each test includes, not from a list someone keeps in
    # step. The first version of this was a list of names written by hand and
    # it was wrong on the first build: four tests that needed the perimeter
    # were not on it, and two that were on it did not exist.
    set(LRVX_VENUE_PERIMETER_HEADERS
        socket_acceptor.h tcp_gateway.h ws_gateway.h tls_gateway.h udp_multicast.h
        session.h session_registry.h cancel_on_disconnect.h fix_session.h
        md_distribution.h md_recovery.h control_server.h control_api.h
        metrics_server.h rest_json.h)
    set(LRVX_VENUE_ENGINE_TESTS "")
    set(LRVX_VENUE_DROPPED "")
    foreach(t ${LRVX_VENUE_TESTS})
      file(STRINGS ${t} _includes REGEX "^#include \"lrvx-venue/")
      set(_needs_perimeter FALSE)
      foreach(h ${LRVX_VENUE_PERIMETER_HEADERS})
        foreach(line ${_includes})
          if(line MATCHES "lrvx-venue/${h}\"")
            set(_needs_perimeter TRUE)
          endif()
        endforeach()
      endforeach()
      if(_needs_perimeter)
        get_filename_component(_n ${t} NAME_WE)
        list(APPEND LRVX_VENUE_DROPPED ${_n})
      else()
        list(APPEND LRVX_VENUE_ENGINE_TESTS ${t})
      endif()
    endforeach()
    set(LRVX_VENUE_TESTS ${LRVX_VENUE_ENGINE_TESTS})
    list(LENGTH LRVX_VENUE_DROPPED _n_dropped)
    message(STATUS "LRVX_VENUE_PERIMETER=OFF: ${_n_dropped} tests need the perimeter: ${LRVX_VENUE_DROPPED}")
  endif()
  # The dependency is declared on the target now (see above), so a test that
  # uses the gateway needs nothing special -- it comes through lrvx-venue.
  if(NOT LRVX_VENUE_TLS)
    list(REMOVE_ITEM LRVX_VENUE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_venue_tls.cpp)
    message(STATUS "OpenSSL not found -- skipping venue TLS gateway test")
  endif()
  if(NOT LRVX_ENABLE_BACKTEST)
    # Same derivation as the two blocks above: read what each test includes
    # rather than keep a list of names in step by hand. The venue library
    # needs nothing from lrvx/backtest/ any more, but liquidation_monitor.h
    # -- a header-only helper over the backtest liquidation engine, used by
    # one test and nothing else -- still does.
    set(LRVX_VENUE_BACKTEST_TESTS "")
    foreach(t ${LRVX_VENUE_TESTS})
      file(STRINGS ${t} _bt REGEX "lrvx/backtest/|liquidation_monitor.h")
      if(_bt)
        list(REMOVE_ITEM LRVX_VENUE_TESTS ${t})
        get_filename_component(_n ${t} NAME_WE)
        list(APPEND LRVX_VENUE_BACKTEST_TESTS ${_n})
      endif()
    endforeach()
    list(LENGTH LRVX_VENUE_BACKTEST_TESTS _n_backtest)
    message(STATUS "LRVX_ENABLE_BACKTEST=OFF: ${_n_backtest} venue tests need the backtest module: ${LRVX_VENUE_BACKTEST_TESTS}")
  endif()

  if(LRVX_VENUE_LITE)
    # The lite profile builds the core library without src/replay, so the
    # tape recorder -- a header-only writer over lrvx::replay::BinaryLogWriter,
    # and not on the consumer's surface -- has no implementation to link
    # against. Derived from includes for the same reason as the blocks above.
    set(LRVX_VENUE_REPLAY_TESTS "")
    foreach(t ${LRVX_VENUE_TESTS})
      file(STRINGS ${t} _rp REGEX "lrvx/replay/|tape_recorder.h")
      if(_rp)
        list(REMOVE_ITEM LRVX_VENUE_TESTS ${t})
        get_filename_component(_n ${t} NAME_WE)
        list(APPEND LRVX_VENUE_REPLAY_TESTS ${_n})
      endif()
    endforeach()
    list(LENGTH LRVX_VENUE_REPLAY_TESTS _n_replay)
    message(STATUS "LRVX_VENUE_LITE: ${_n_replay} venue tests need the replay writers: ${LRVX_VENUE_REPLAY_TESTS}")
  endif()

  # The surface contract for the lite profile: one TU including exactly what
  # the consumer includes. Built here and not in the full profile, because
  # the point is what it compiles WITHOUT -- in a full configure the backtest
  # module and the connectors are present and the TU would pass whether or
  # not the lite profile still holds. scripts/lite_closure.py (run by the
  # lite configure) checks its includes against venue/lite_surface.txt.
  if(LRVX_VENUE_LITE)
    add_executable(venue_lite_surface
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/lite_surface.cpp)
    target_link_libraries(venue_lite_surface PRIVATE lrvx-venue)
    target_include_directories(venue_lite_surface PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # The half of the contract the compiler cannot see: that the TU's
    # includes are exactly the consumer's list, and that the closure of that
    # list has not grown a direction the profile forbids. Attached to this
    # target rather than left to CI, so an edit to venue/lite_surface.txt
    # that the TU does not match reddens the surface target on the developer's
    # own build.
    add_custom_target(venue_lite_surface_contract
      COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/lite_closure.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
      COMMENT "Checking the venue-lite surface list against its translation unit")
    add_dependencies(venue_lite_surface venue_lite_surface_contract)
    add_test(NAME venue_lite_surface COMMAND venue_lite_surface)
  endif()

  # A shard that writes a journal and then abandons the process. Used by the
  # process-death test on every platform: Windows has no fork, and a
  # Windows-only mechanism would be exercised only where nobody runs it by
  # hand. Excluded from the test list -- it has a main(), not a TEST().
  list(REMOVE_ITEM LRVX_VENUE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/journal_writer_main.cpp)
  add_executable(venue_journal_writer ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/journal_writer_main.cpp)
  target_link_libraries(venue_journal_writer PRIVATE lrvx-venue)
  target_include_directories(venue_journal_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  foreach(test_src ${LRVX_VENUE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name}
      PRIVATE lrvx-venue GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main)
    if(TARGET Threads::Threads)
      target_link_libraries(${test_name} PRIVATE Threads::Threads)
    endif()
    if(test_name STREQUAL "test_venue_golden_replay")
      # The table of recorded hashes, by source path rather than a guess at
      # the build layout: the test reads it, and LRVX_UPDATE_GOLDEN rewrites
      # it IN THE SOURCE TREE so the new numbers land in a diff a reviewer
      # sees. A copy staged into the build directory would be updated where
      # nobody looks.
      target_compile_definitions(${test_name}
        PRIVATE LRVX_VENUE_GOLDEN_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/replay_hashes.txt")
    endif()
    if(test_name STREQUAL "test_venue_engine_session")
      # The session transition table generates its own row of documentation and
      # this test checks the committed block against it, so the written
      # automaton cannot drift from the running one. By source path, like the
      # golden table: LRVX_UPDATE_SESSION_TABLE=1 rewrites it IN THE SOURCE
      # TREE, where a reviewer sees the diff.
      target_compile_definitions(${test_name}
        PRIVATE LRVX_VENUE_SESSION_DOC="${CMAKE_CURRENT_SOURCE_DIR}/../docs/venue/matching.md")
    endif()
    if(test_name STREQUAL "test_venue_recovery")
      # The helper's path rather than a guess at the build layout: a test that
      # looked for a sibling binary by name would find the wrong one, or none,
      # depending on the generator.
      target_compile_definitions(${test_name}
        PRIVATE LRVX_JOURNAL_WRITER="$<TARGET_FILE:venue_journal_writer>")
      add_dependencies(${test_name} venue_journal_writer)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
