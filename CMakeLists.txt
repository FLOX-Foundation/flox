cmake_minimum_required(VERSION 3.22)
project(flox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_DEV_SETUP "Install pre-commit hook" OFF)

if(ENABLE_DEV_SETUP)
  message(STATUS "Developer setup enabled")

  set(PRECOMMIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/scripts/pre-commit")
  set(PRECOMMIT_DST "${CMAKE_CURRENT_SOURCE_DIR}/.git/hooks/pre-commit")

  if(EXISTS "${PRECOMMIT_SRC}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PRECOMMIT_SRC}" "${PRECOMMIT_DST}")
    execute_process(COMMAND chmod +x "${PRECOMMIT_DST}")
    message(STATUS "Installed pre-commit hook to .git/hooks")
  else()
    message(WARNING "pre-commit script not found at ${PRECOMMIT_SRC}")
  endif()
endif()

include_directories(include)

file(GLOB_RECURSE SRC_FILES
    src/**/*.cpp
)

set(FLOX ${PROJECT_NAME})
set(FLOX_SYNC ${PROJECT_NAME}_sync)

add_library(${FLOX} STATIC ${SRC_FILES})
add_library(flox::${FLOX} ALIAS ${FLOX})
target_compile_options(${FLOX} PUBLIC
  $<$<CONFIG:Release>:-O3 -march=native -flto -funroll-loops>
)

add_library(${FLOX_SYNC} STATIC ${SRC_FILES})
add_library(flox::${FLOX_SYNC} ALIAS ${FLOX_SYNC})
target_compile_definitions(${FLOX_SYNC} PUBLIC USE_SYNC_BOOK_UPDATE_BUS USE_SYNC_TRADE_BUS USE_SYNC_ORDER_BUS USE_SYNC_CANDLE_BUS)
target_compile_options(${FLOX_SYNC} PUBLIC
  $<$<CONFIG:Release>:-O3 -march=native -flto -funroll-loops>
)

# WARNING: CPU affinity can decrease performance on busy/shared systems.
# It prevents OS scheduler optimization and should only be used on isolated,
# dedicated hardware where you control the entire system workload.
option(ENABLE_CPU_AFFINITY "Enable CPU affinity and NUMA functionality" OFF)
if(ENABLE_CPU_AFFINITY)
  message(STATUS "CPU affinity features enabled")
  target_compile_definitions(${FLOX} PUBLIC FLOX_CPU_AFFINITY_ENABLED=1)
  target_compile_definitions(${FLOX_SYNC} PUBLIC FLOX_CPU_AFFINITY_ENABLED=1)
  # Find and conditionally link NUMA library for CPU affinity functionality
  find_library(NUMA_LIBRARY numa)
  if(NUMA_LIBRARY)
    target_link_libraries(${FLOX} PUBLIC ${NUMA_LIBRARY})
    target_link_libraries(${FLOX_SYNC} PUBLIC ${NUMA_LIBRARY})
    target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=1)
    target_compile_definitions(${FLOX_SYNC} PUBLIC FLOX_NUMA_LIBRARY_LINKED=1)
    message(STATUS "Found NUMA library: ${NUMA_LIBRARY}")
  else()
    target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
    target_compile_definitions(${FLOX_SYNC} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
    message(WARNING "NUMA library not found")
  endif()
else()
  message(STATUS "CPU affinity features disabled")
  target_compile_definitions(${FLOX} PUBLIC FLOX_CPU_AFFINITY_ENABLED=0)
  target_compile_definitions(${FLOX_SYNC} PUBLIC FLOX_CPU_AFFINITY_ENABLED=0)
  target_compile_definitions(${FLOX} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
  target_compile_definitions(${FLOX_SYNC} PUBLIC FLOX_NUMA_LIBRARY_LINKED=0)
endif()

option(ENABLE_BENCHMARK "Enable benchmarks" OFF)
if (ENABLE_BENCHMARK)
  add_subdirectory(benchmarks)
endif()


option(ENABLE_TESTS "Enable tests" OFF)
if (ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

option(ENABLE_DEMO "Build demo application" OFF)
if (ENABLE_DEMO)
  add_subdirectory(demo)
endif()

target_include_directories(flox PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

install(TARGETS flox
  EXPORT floxTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

install(EXPORT floxTargets
  FILE floxTargets.cmake
  NAMESPACE flox::
  DESTINATION lib/cmake/flox
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/floxConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfig.cmake"
  INSTALL_DESTINATION lib/cmake/flox
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/floxConfigVersion.cmake"
  DESTINATION lib/cmake/flox
)
