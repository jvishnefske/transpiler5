# The C importer library: clang-LibTooling-based translation from a C11
# subset to the hybrid core+EmitRust MLIR representation. The clang
# dependencies stay PRIVATE so that no other EmitRust library or tool picks
# them up transitively.
add_mlir_library(MLIREmitRustImportC
  ImportC.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust

  DEPENDS
  MLIREmitRustOpsIncGen
  MLIREmitRustAttributesIncGen
  MLIREmitRustEnumsIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRFuncDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRControlFlowDialect
  MLIRIR
  MLIRSupport
  PRIVATE
  clangTooling
  clangFrontend
  clangSerialization
  clangAST
  clangLex
  clangBasic
  )
