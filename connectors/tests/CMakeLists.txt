# Connector integration tests. Build-only by default — they hit live
# exchange endpoints, so running them in CI without credentials is
# pointless. Pass -DFLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON to
# generate them; pass -DFLOX_RUN_CONNECTOR_INTEGRATION_TESTS=ON to
# also register them with ctest.

find_package(GTest REQUIRED)

option(FLOX_RUN_CONNECTOR_INTEGRATION_TESTS
       "Register connector integration tests with ctest" OFF)

file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS unit_test_*.cpp)
set(TEST_SOURCES ${UNIT_TEST_SOURCES})
if(FLOX_BUILD_CONNECTOR_INTEGRATION_TESTS)
    file(GLOB INTEGRATION_TEST_SOURCES CONFIGURE_DEPENDS integration_test_*.cpp)
    list(APPEND TEST_SOURCES ${INTEGRATION_TEST_SOURCES})
endif()

foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)

    add_executable(${test_name} "${test_src}")

    target_link_libraries(${test_name}
        PRIVATE
            flox-connectors
            flox::flox
            GTest::gtest
            GTest::gtest_main
            ixwebsocket
            simdjson::simdjson
    )

    target_compile_features(${test_name} PRIVATE cxx_std_23)

    # unit_test_* are offline (no network) and always registered; the
    # integration_test_* ones hit live exchanges and stay opt-in.
    if(test_name MATCHES "^unit_test_")
        add_test(NAME ${test_name} COMMAND ${test_name})
    elseif(FLOX_RUN_CONNECTOR_INTEGRATION_TESTS)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()
