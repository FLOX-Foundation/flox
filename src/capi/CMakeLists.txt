add_library(flox_capi SHARED flox_capi.cpp)

target_link_libraries(flox_capi PRIVATE flox)
target_include_directories(flox_capi PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(flox_capi PROPERTIES
  C_VISIBILITY_PRESET default
  CXX_VISIBILITY_PRESET default
  POSITION_INDEPENDENT_CODE ON
)

if(NOT MSVC)
  # FLOX_ARCH_OPT comes from the top-level CMakeLists.txt and honours
  # the FLOX_NATIVE option; the shared CAPI lib is a distribution target
  # (Codon/QuickJS/Node load it at runtime), so set FLOX_NATIVE=OFF when
  # shipping it to heterogeneous CPUs.
  set(_FLOX_CAPI_OPTS -O3 -flto)
  if(FLOX_ARCH_OPT)
    list(APPEND _FLOX_CAPI_OPTS ${FLOX_ARCH_OPT})
  endif()
  target_compile_options(flox_capi PRIVATE
    $<$<CONFIG:Release>:${_FLOX_CAPI_OPTS}>
  )
endif()

install(TARGETS flox_capi
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(FILES ${CMAKE_SOURCE_DIR}/include/flox/capi/flox_capi.h
  DESTINATION include/flox/capi
)
