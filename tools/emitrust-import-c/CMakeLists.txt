# =============================================================================
# tools/emitrust-import-c/CMakeLists.txt
# Builds emitrust-import-c, the C-to-EmitRust importer driver: it parses a C
# file with the MLIREmitRustImportC library and prints the resulting hybrid
# core+EmitRust module. The clang libraries stay encapsulated inside
# MLIREmitRustImportC; the tool links MLIR libraries only.
# =============================================================================

set(LLVM_LINK_COMPONENTS
  Support
  )

set(LIBS
  MLIREmitRustImportC
  MLIREmitRustDialect
  MLIRFuncDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRControlFlowDialect
  MLIRIR
  MLIRSupport
  )

add_llvm_executable(emitrust-import-c emitrust-import-c.cpp)
llvm_update_compile_flags(emitrust-import-c)
target_link_libraries(emitrust-import-c PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-import-c)
