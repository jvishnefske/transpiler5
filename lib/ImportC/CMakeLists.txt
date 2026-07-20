# The C importer library: clang-LibTooling-based translation from a C11
# subset to the hybrid core+EmitRust MLIR representation. The clang
# dependencies stay PRIVATE so that no other EmitRust library or tool picks
# them up transitively.
add_mlir_library(MLIREmitRustImportC
  ImportC.cpp
  ImportCTypes.cpp
  ImportCAggregates.cpp
  ImportCGlobals.cpp
  ImportCFunctions.cpp

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

# Discover clang's builtin-header resource dir at configure time so the
# importer can pass `-resource-dir` and resolve system headers such as
# <stdint.h>. This is baked into the library as the EMITRUST_CLANG_RESOURCE_DIR
# compile define; the importer also honors the EMITRUST_RESOURCE_DIR
# environment variable at run time, which takes precedence. If clang is not
# found the define is simply omitted (the build never breaks) and system
# headers are only available when EMITRUST_RESOURCE_DIR is set.
find_program(EMITRUST_CLANG_EXECUTABLE NAMES clang)
if(EMITRUST_CLANG_EXECUTABLE)
  execute_process(
    COMMAND ${EMITRUST_CLANG_EXECUTABLE} -print-resource-dir
    OUTPUT_VARIABLE EMITRUST_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE EMITRUST_CLANG_RESOURCE_RESULT)
  if(EMITRUST_CLANG_RESOURCE_RESULT EQUAL 0 AND EMITRUST_CLANG_RESOURCE_DIR)
    target_compile_definitions(MLIREmitRustImportC PRIVATE
      "EMITRUST_CLANG_RESOURCE_DIR=\"${EMITRUST_CLANG_RESOURCE_DIR}\"")
    message(STATUS
      "Using clang resource dir: ${EMITRUST_CLANG_RESOURCE_DIR}")
  endif()
endif()
