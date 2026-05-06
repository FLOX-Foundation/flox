# Connector integration tests. Build-only by default — they hit live
# exchange endpoints, so running them in CI without credentials is
# pointless. Pass -DFLOX_BUILD_CONNECTOR_INTEGRATION_TESTS=ON to
# generate them; pass -DFLOX_RUN_CONNECTOR_INTEGRATION_TESTS=ON to
# also register them with ctest.

find_package(GTest REQUIRED)

option(FLOX_RUN_CONNECTOR_INTEGRATION_TESTS
       "Register connector integration tests with ctest" OFF)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

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

    if(FLOX_RUN_CONNECTOR_INTEGRATION_TESTS)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()
