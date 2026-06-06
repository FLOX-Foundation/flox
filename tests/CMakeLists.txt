find_package(GTest REQUIRED)
find_package(Threads)

function(add_flox_test name)
  add_executable(${name} ${name}.cpp)
  target_compile_definitions(${name} PRIVATE FLOX_UNIT_TEST)
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
  )
  if(TARGET Threads::Threads)
    target_link_libraries(${name} PRIVATE Threads::Threads)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_flox_test(test_book_update_bus)
add_flox_test(test_bar_aggregator)
add_flox_test(test_connection_factory)
add_flox_test(test_connector_manager)
add_flox_test(test_decimal)
add_flox_test(test_scale)
add_flox_test(test_lp_signal)
add_flox_test(test_engine_smoke)
add_flox_test(test_event_pool)
add_flox_test(test_l3_order_book)
add_flox_test(test_multi_execution_listener)
add_flox_test(test_nlevel_order_book)
add_flox_test(test_order_execution_bus)
add_flox_test(test_order_lifecycle)
add_flox_test(test_onchain_lifecycle)
add_flox_test(test_push_pull_subscribers)
add_flox_test(test_ref_countable)
add_flox_test(test_spsc_advanced)
add_flox_test(test_spsc)
add_flox_test(test_symbol_registry)
add_flox_test(test_atomic_logger)
add_flox_test(test_order_tracker)
add_flox_test(test_rate_limiter)

add_flox_test(test_sync_order_execution_bus)
add_flox_test(test_sync_book_update_bus)
add_flox_test(test_sync_bar_aggregator)

# EventBus core tests
add_flox_test(test_event_bus)

# Replay tests
add_flox_test(test_binary_log)
add_flox_test(test_replay_connector)
add_flox_test(test_aggregator_framework)

# Run-trace tests (.floxrun)
add_flox_test(test_run_trace)

# Multi-TF alignment helpers
add_flox_test(test_multi_tf_alignment)

# Run-trace adapter handlers (.floxrun auto-capture)
add_flox_test(test_trace_handlers)

# Bar-close ordering on tied timestamps
add_flox_test(test_bar_close_ordering)

add_flox_test(test_multi_symbol_strategy)

add_flox_test(test_order_group_replace)

add_flox_test(test_position_aggregator)
add_flox_test(test_position_valuator)

# CEX coordination tests
add_flox_test(test_cex_coordination)

if(FLOX_ENABLE_BACKTEST)
  add_flox_test(test_backtest)
  add_flox_test(test_interactive_runner)
  add_flox_test(test_advanced_orders)
  add_flox_test(test_mmap_bar_storage)
  add_flox_test(test_optimization_stats)
  add_flox_test(test_grid_search)
  add_flox_test(test_backtest_slippage)
  add_flox_test(test_backtest_queue)
  add_flox_test(test_order_timestamps)
  add_flox_test(test_maker_taker)
  add_flox_test(test_cancel_latency)
  add_flox_test(test_submit_ack_latency)
  add_flox_test(test_latency_profiles)
  add_flox_test(test_latency_distribution)
  add_flox_test(test_order_journey_tracer)
  add_flox_test(test_market_position)
  add_flox_test(test_replace_in_flight)
  add_flox_test(test_backtest_metrics)
  add_flox_test(test_backtest_ctx_position)
  add_flox_test(test_backtest_run_tape)
  add_flox_test(test_backtest_runner_hooks)
  add_flox_test(test_live_queue_position)
  add_flox_test(test_extended_tif)
  add_flox_test(test_hidden_order_attribution)
  add_flox_test(test_funding_schedule)
  add_flox_test(test_rate_limit_policy)
  add_flox_test(test_fee_schedule)
  add_flox_test(test_stp_modes)
  add_flox_test(test_pro_rata_matching)
  add_flox_test(test_venue_availability)
  add_flox_test(test_bracket_orders)
  add_flox_test(test_liquidation_engine)
  add_flox_test(test_w15_composition)
  add_flox_test(test_iceberg_orders)
  add_flox_test(test_account)
  add_flox_test(test_venue_stack)
  add_flox_test(test_w15_realism_integration)
  add_flox_test(test_cross_cascade)
  add_flox_test(test_w15_reproducibility)
  add_flox_test(test_w15_calibration)
  add_flox_test(test_option_settlement)
  add_flox_test(test_option_exercise)
  add_flox_test(test_option_fill_model)
endif()

add_flox_test(test_indicators)
add_flox_test(test_streaming_parity)
add_flox_test(test_targets)
add_flox_test(test_adf)
add_flox_test(test_autocorrelation)
add_flox_test(test_portfolio_greeks)
add_flox_test(test_delta_hedger)
add_flox_test(test_hedge_cost)
add_flox_test(test_hedge_templates)
add_flox_test(test_black_scholes)
add_flox_test(test_greeks)
add_flox_test(test_american_pricing)
add_flox_test(test_svi_surface)
add_flox_test(test_carry)
add_flox_test(test_trading_calendar)
add_flox_test(test_vol_cone)
add_flox_test(test_option_quote_record)
add_flox_test(test_option_quote_io)

if(FLOX_BUILD_CAPI)
  add_flox_test(test_capi_registry)
  target_link_libraries(test_capi_registry PRIVATE flox_capi)
  add_flox_test(test_capi_infra)
  target_link_libraries(test_capi_infra PRIVATE flox_capi)
  add_flox_test(test_capi_risk_manager)
  target_link_libraries(test_capi_risk_manager PRIVATE flox_capi)
  add_flox_test(test_capi_kill_switch)
  target_link_libraries(test_capi_kill_switch PRIVATE flox_capi)
  add_flox_test(test_capi_observers)
  target_link_libraries(test_capi_observers PRIVATE flox_capi)
  add_flox_test(test_capi_market_data_recorder)
  target_link_libraries(test_capi_market_data_recorder PRIVATE flox_capi)
  add_flox_test(test_capi_replay_source)
  target_link_libraries(test_capi_replay_source PRIVATE flox_capi)
  add_flox_test(test_capi_execution_listener)
  target_link_libraries(test_capi_execution_listener PRIVATE flox_capi)
  add_flox_test(test_capi_executor)
  target_link_libraries(test_capi_executor PRIVATE flox_capi)
  # test_capi_logger needs to share a single ILogger global with flox_capi.
  # If we use add_flox_test, the test exe links the static `flox` library
  # alongside flox_capi.dylib (which itself statically embeds `flox`) —
  # two copies of g_logger, so flox_set_log_callback in the dylib doesn't
  # affect FLOX_LOG_* calls compiled into the test exe. Build manually to
  # link only flox_capi (which exports every symbol the test reaches).
  add_executable(test_capi_logger test_capi_logger.cpp)
  target_compile_definitions(test_capi_logger PRIVATE FLOX_UNIT_TEST)
  target_link_libraries(test_capi_logger
    PRIVATE flox_capi
    GTest::gtest
    GTest::gtest_main
  )
  if(TARGET Threads::Threads)
    target_link_libraries(test_capi_logger PRIVATE Threads::Threads)
  endif()
  add_test(NAME test_capi_logger COMMAND test_capi_logger)
  if(FLOX_ENABLE_BACKTEST)
    add_flox_test(test_capi_backtest)
    target_link_libraries(test_capi_backtest PRIVATE flox_capi)
  endif()
endif()

if(FLOX_BUILD_QUICKJS)
  add_flox_test(test_quickjs)
  target_link_libraries(test_quickjs PRIVATE flox_quickjs)
endif()

if(FLOX_ENABLE_CPU_AFFINITY)
    add_flox_test(test_cpu_affinity)
endif()
