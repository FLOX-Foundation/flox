add_library(flox_capi SHARED flox_capi.cpp)

target_link_libraries(flox_capi PRIVATE flox)
target_include_directories(flox_capi PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(flox_capi PROPERTIES
  C_VISIBILITY_PRESET default
  CXX_VISIBILITY_PRESET default
  POSITION_INDEPENDENT_CODE ON
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(NOT MSVC)
  target_compile_options(flox_capi PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -flto>
  )
endif()

install(TARGETS flox_capi
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/flox/capi/flox_capi.h
  DESTINATION include/flox/capi
)
