find_package(pybind11 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development NumPy)

pybind11_add_module(flox_py flox_py.cpp)

target_link_libraries(flox_py PRIVATE flox)
target_include_directories(flox_py PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${Python3_NumPy_INCLUDE_DIRS}
)

if(NOT MSVC)
  target_compile_options(flox_py PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -flto>
  )
endif()

install(TARGETS flox_py LIBRARY DESTINATION . COMPONENT python_module)
