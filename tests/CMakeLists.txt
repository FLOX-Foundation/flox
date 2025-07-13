find_package(GTest REQUIRED)

function(add_flox_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name}
    PRIVATE
        include()
  )
  target_link_libraries(${name}
    PRIVATE ${FLOX}
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    GTest::gmock_main
    Threads::Threads
  )
  add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_flox_sync_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name}
    PRIVATE
        include
  )
  target_link_libraries(${name}
    PRIVATE ${FLOX_SYNC}
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    GTest::gmock_main
    Threads::Threads
  )
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_flox_test(test_book_update_bus)
add_flox_test(test_candle_aggregator)
add_flox_test(test_connection_factory)
add_flox_test(test_connector_manager)
add_flox_test(test_decimal)
add_flox_test(test_engine_smoke)
add_flox_test(test_event_bus_instantiation)
add_flox_test(test_event_pool)
add_flox_test(test_multi_execution_listener)
add_flox_test(test_nlevel_order_book)
add_flox_test(test_order_execution_bus)
add_flox_test(test_order_lifecycle)
add_flox_test(test_push_pull_subscribers)
add_flox_test(test_ref_countable)
add_flox_test(test_spsc_advanced)
add_flox_test(test_spsc)
add_flox_test(test_symbol_registry)
add_flox_test(test_tick_barrier)
add_flox_test(test_atomic_logger)

if(ENABLE_CPU_AFFINITY)
    add_flox_test(test_cpu_affinity)
endif()

add_flox_sync_test(test_sync_order_execution_bus)
add_flox_sync_test(test_sync_book_update_bus)
add_flox_sync_test(test_sync_candle_aggregator)