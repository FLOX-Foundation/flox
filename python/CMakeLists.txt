find_package(pybind11 REQUIRED)

# Compiled module name is `_flox_py`; the user-facing import path
# `flox_py` is provided by the pure-Python wrapper in flox_py/__init__.py
# that re-exports everything. This split lets us ship .pyi stubs and a
# PEP 561 `py.typed` marker alongside the .so in one importable package.
pybind11_add_module(_flox_py flox_py.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi/flox_capi.cpp)

target_link_libraries(_flox_py PRIVATE flox)
target_include_directories(_flox_py PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/capi
)

if(NOT MSVC)
  target_compile_options(_flox_py PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -flto>
  )
endif()

# Place the compiled .so inside the package directory in the build tree
# so `PYTHONPATH=build/python python -c "import flox_py"` works during
# local development and CI without an install step.
set_target_properties(_flox_py PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/flox_py
)

# Mirror the source-side package layout (pure-Python wrapper, py.typed
# marker, generated stubs) into the build tree alongside the .so. The
# wrapper stub `flox_py/__init__.pyi` lives at the package root; the
# C-extension stubs sit under `flox_py/_flox_py/` because that is where
# type checkers resolve `flox_py._flox_py.X`. Stubs are copied only if
# present so the very first build — which produces the .so that stubgen
# needs as input — is not blocked on them existing.
#
# The .py file list is globbed from `flox_py/*.py` so a new module
# automatically lands in the wheel without touching this file. CMake
# re-runs the configure step when this CMakeLists.txt changes; if you
# add a new .py module, run `cmake -B build -S .` once to pick it up.
file(GLOB FLOX_PY_PURE_PYTHON_MODULES
     CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/flox_py/*.py")
set(FLOX_PY_REQUIRED_FILES
  ${FLOX_PY_PURE_PYTHON_MODULES}
  ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/py.typed
)
set(FLOX_PY_OPTIONAL_ROOT_STUBS
  ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/__init__.pyi
)
set(FLOX_PY_OPTIONAL_EXT_STUBS
  ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/_flox_py/__init__.pyi
  ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/_flox_py/targets.pyi
)
add_custom_command(TARGET _flox_py POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/flox_py
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/flox_py/_flox_py
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${FLOX_PY_REQUIRED_FILES}
    ${CMAKE_CURRENT_BINARY_DIR}/flox_py/
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/templates
    ${CMAKE_CURRENT_BINARY_DIR}/flox_py/templates
)
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    add_custom_command(TARGET _flox_py POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${opt_file}
        ${CMAKE_CURRENT_BINARY_DIR}/flox_py/
    )
  endif()
endforeach()
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    add_custom_command(TARGET _flox_py POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${opt_file}
        ${CMAKE_CURRENT_BINARY_DIR}/flox_py/_flox_py/
    )
  endif()
endforeach()

# A separate ALL target keeps the build-tree stubs in sync with source
# even when the .so itself does not need rebuilding (editing only the
# stubs after `cmake --build` would otherwise leave stale .pyi in the
# build tree, causing CI's `gen_pyi_stubs.py --check` to compare against
# an outdated copy).
set(FLOX_PY_BUILD_STUB_TARGETS "")
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    get_filename_component(basename ${opt_file} NAME)
    set(dst ${CMAKE_CURRENT_BINARY_DIR}/flox_py/${basename})
    add_custom_command(OUTPUT ${dst}
      COMMAND ${CMAKE_COMMAND} -E copy ${opt_file} ${dst}
      DEPENDS ${opt_file}
    )
    list(APPEND FLOX_PY_BUILD_STUB_TARGETS ${dst})
  endif()
endforeach()
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    get_filename_component(basename ${opt_file} NAME)
    set(dst ${CMAKE_CURRENT_BINARY_DIR}/flox_py/_flox_py/${basename})
    add_custom_command(OUTPUT ${dst}
      COMMAND ${CMAKE_COMMAND} -E copy ${opt_file} ${dst}
      DEPENDS ${opt_file}
    )
    list(APPEND FLOX_PY_BUILD_STUB_TARGETS ${dst})
  endif()
endforeach()
if(FLOX_PY_BUILD_STUB_TARGETS)
  add_custom_target(_flox_py_stubs ALL DEPENDS ${FLOX_PY_BUILD_STUB_TARGETS})
endif()

install(TARGETS _flox_py LIBRARY DESTINATION flox_py COMPONENT python_module)
install(FILES ${FLOX_PY_REQUIRED_FILES}
        DESTINATION flox_py COMPONENT python_module)
# Bundled `flox new` templates. Shipped as data alongside the package
# so `flox new <project>` works after `pip install flox-py`. Walked at
# runtime by flox_py.cli via importlib.resources.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/flox_py/templates/
        DESTINATION flox_py/templates
        COMPONENT python_module)
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_ROOT_STUBS)
  if(EXISTS ${opt_file})
    install(FILES ${opt_file} DESTINATION flox_py COMPONENT python_module)
  endif()
endforeach()
foreach(opt_file IN LISTS FLOX_PY_OPTIONAL_EXT_STUBS)
  if(EXISTS ${opt_file})
    install(FILES ${opt_file} DESTINATION flox_py/_flox_py COMPONENT python_module)
  endif()
endforeach()
