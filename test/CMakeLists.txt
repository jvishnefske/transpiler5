llvm_canonicalize_cmake_booleans(
  MLIR_ENABLE_BINDINGS_PYTHON
)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
  MAIN_CONFIG
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)

set(EMITRUST_TEST_DEPENDS
  emitrust-opt
  emitrust-translate
  emitrust-import-c
  emitrust-cc
  emitrust-clang
)

add_lit_testsuite(check-emitrust "Running the EmitRust regression tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${EMITRUST_TEST_DEPENDS}
)
set_target_properties(check-emitrust PROPERTIES FOLDER "Tests")

add_lit_testsuites(EMITRUST ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${EMITRUST_TEST_DEPENDS}
)
