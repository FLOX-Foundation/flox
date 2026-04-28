find_program(CODON_COMPILER codon HINTS $ENV{CODON_HOME}/bin $ENV{HOME}/.codon/bin)

if(NOT CODON_COMPILER)
  message(WARNING "Codon compiler not found. Skipping Codon build. "
                  "Set CODON_HOME or add codon to PATH.")
  return()
endif()

message(STATUS "Found Codon compiler: ${CODON_COMPILER}")

# Compile a Codon source file to a native executable
function(add_codon_executable name source)
  set(OUTPUT_EXE ${CMAKE_CURRENT_BINARY_DIR}/${name})
  add_custom_command(
    OUTPUT ${OUTPUT_EXE}
    COMMAND ${CMAKE_COMMAND} -E env
      "LIBRARY_PATH=$<TARGET_FILE_DIR:flox_capi>"
      "CODON_PATH=${CMAKE_CURRENT_SOURCE_DIR}"
      ${CODON_COMPILER} build
        -o ${OUTPUT_EXE}
        -l flox_capi
        ${source}
    DEPENDS ${source} flox_capi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Compiling Codon: ${name}"
  )
  add_custom_target(build_${name} ALL DEPENDS ${OUTPUT_EXE})
endfunction()

add_codon_executable(codon_sma_crossover
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/sma_crossover.codon)

add_codon_executable(codon_pairs_strategy
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/pairs_strategy.codon)

add_codon_executable(codon_backtest_from_csv
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/backtest_from_csv.codon)

add_codon_executable(codon_full_backtest
  ${CMAKE_SOURCE_DIR}/docs/examples/codon_full_backtest.codon)

add_codon_executable(codon_graph_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/examples/graph_smoke.codon)
