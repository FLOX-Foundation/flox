find_package(pybind11 REQUIRED)

pybind11_add_module(flox_py flox_py.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi/flox_capi.cpp)

target_link_libraries(flox_py PRIVATE flox)
target_include_directories(flox_py PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi
)

if(NOT MSVC)
  target_compile_options(flox_py PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -flto>
  )
endif()

install(TARGETS flox_py LIBRARY DESTINATION . COMPONENT python_module)
