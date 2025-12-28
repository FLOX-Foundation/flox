# Flox Tools
# Command-line utilities for data processing

if(FLOX_ENABLE_BACKTEST)
    add_executable(preagg_bars preagg_bars.cpp)
    target_link_libraries(preagg_bars PRIVATE flox)
    target_include_directories(preagg_bars PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()
