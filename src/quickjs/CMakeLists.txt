# QuickJS — vendored sources from third_party/quickjs
set(QJS_DIR ${CMAKE_SOURCE_DIR}/third_party/quickjs)
set(QJS_SOURCES
  ${QJS_DIR}/quickjs.c
  ${QJS_DIR}/libregexp.c
  ${QJS_DIR}/libunicode.c
  ${QJS_DIR}/cutils.c
  ${QJS_DIR}/libbf.c
)

add_library(quickjs_lib STATIC ${QJS_SOURCES})
target_include_directories(quickjs_lib PUBLIC ${QJS_DIR})
set_target_properties(quickjs_lib PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)
target_compile_definitions(quickjs_lib PRIVATE
  CONFIG_VERSION="flox"
  CONFIG_BIGNUM=1
)
if(UNIX)
  target_link_libraries(quickjs_lib PRIVATE m)
endif()

# Generate embedded JS stdlib as C string literals via a script
# Rebuilds when JS source files change
set(JS_STDLIB_DIR ${CMAKE_SOURCE_DIR}/quickjs/flox)
set(GEN_INC_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/gen_js_inc.cmake)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/strategy.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/strategy.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding strategy.js"
)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
  COMMAND ${CMAKE_COMMAND}
    -DIN_FILE=${JS_STDLIB_DIR}/indicators.js
    -DOUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
    -P ${GEN_INC_SCRIPT}
  DEPENDS ${JS_STDLIB_DIR}/indicators.js ${GEN_INC_SCRIPT}
  COMMENT "Embedding indicators.js"
)

add_custom_target(js_stdlib_gen DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_strategy.inc
  ${CMAKE_CURRENT_BINARY_DIR}/js_stdlib_indicators.inc
)

# flox_quickjs static library
add_library(flox_quickjs STATIC
  js_engine.cpp
  js_bindings.cpp
  js_strategy.cpp
)
add_dependencies(flox_quickjs js_stdlib_gen)
target_include_directories(flox_quickjs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(flox_quickjs PUBLIC
  quickjs_lib
  flox_capi
  ${FLOX}
)
target_compile_features(flox_quickjs PRIVATE cxx_std_20)

# flox_js_runner executable
add_executable(flox_js_runner js_runner.cpp)
target_link_libraries(flox_js_runner PRIVATE flox_quickjs)
target_compile_features(flox_js_runner PRIVATE cxx_std_20)
