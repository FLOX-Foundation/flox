# Flox Tools
# Command-line utilities for data processing

if(FLOX_ENABLE_BACKTEST)
    add_executable(preagg_bars preagg_bars.cpp)
    target_link_libraries(preagg_bars PRIVATE flox)
    target_include_directories(preagg_bars PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

# Receive-path integration probe (POSIX sockets; AF_XDP parts compile only
# with FLOX_ENABLE_AF_XDP). Not built on Windows.
if(NOT WIN32)
  add_executable(afxdp_probe afxdp_probe.cpp)
  target_link_libraries(afxdp_probe PRIVATE flox)
  target_include_directories(afxdp_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()
