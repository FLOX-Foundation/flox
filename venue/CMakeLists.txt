# ── flox venue module ────────────────────────────────────────────────
# The venue side of flox: build a market rather than trade on one. Kept as an
# optional module (mirroring `connectors/`) so the core library stays free of
# its weight, and so the server perimeter can carry its own dependencies
# without pushing them onto every flox user.
#
# Namespace: flox::venue.  Include prefix: <flox-venue/...>.

file(GLOB_RECURSE FLOX_VENUE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(flox-venue STATIC ${FLOX_VENUE_SRC})
add_library(flox::venue ALIAS flox-venue)

target_include_directories(flox-venue
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(flox-venue PUBLIC ${FLOX})
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
  find_package(GTest REQUIRED)
  find_package(Threads)
  file(GLOB FLOX_VENUE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
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
