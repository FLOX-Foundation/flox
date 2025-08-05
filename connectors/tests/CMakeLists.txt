file(GLOB TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)

    add_executable(${test_name} "${test_src}")

    target_link_libraries(${test_name}
        PRIVATE
            flox-connectors
            flox::flox
            gtest gtest_main
            ixwebsocket
            simdjson::simdjson
    )

    target_compile_features(${test_name} PRIVATE cxx_std_23)

    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
