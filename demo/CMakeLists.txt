add_executable(flox_demo
    src/main.cpp
    src/demo_connector.cpp
    src/demo_strategy.cpp
    src/demo_builder.cpp
)

target_include_directories(flox_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                            ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(flox_demo PRIVATE ${FLOX})

if(FLOX_ENABLE_BACKTEST)
  add_executable(backtest_demo src/backtest_demo.cpp)
  target_include_directories(backtest_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(backtest_demo PRIVATE ${FLOX})

  add_executable(grid_search_demo src/grid_search_demo.cpp)
  target_include_directories(grid_search_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                                      ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(grid_search_demo PRIVATE ${FLOX})
endif()

# Bar aggregator demos
add_executable(multi_timeframe_demo src/multi_timeframe_demo.cpp)
target_include_directories(multi_timeframe_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(multi_timeframe_demo PRIVATE ${FLOX})

add_executable(volume_profile_demo src/volume_profile_demo.cpp)
target_include_directories(volume_profile_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(volume_profile_demo PRIVATE ${FLOX})

add_executable(footprint_demo src/footprint_demo.cpp)
target_include_directories(footprint_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(footprint_demo PRIVATE ${FLOX})

add_executable(market_profile_demo src/market_profile_demo.cpp)
target_include_directories(market_profile_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(market_profile_demo PRIVATE ${FLOX})

# CEX (Cross-Exchange Coordination) demo
add_executable(cex_demo src/cex_demo.cpp)
target_include_directories(cex_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(cex_demo PRIVATE ${FLOX})
