# =============================================================================
# tools/emitrust-clang/CMakeLists.txt
# Builds emitrust-clang, the FR-56 compiler-shim front end: a drop-in CC that
# delegates every invocation to the real clang (so builds, probes, and links
# behave identically) and side-emits one <object>.emitrust.mlirbc bytecode
# artifact per compiled C translation unit, embedded into the object file as
# its .emitrust section (FR-57b; ObjCopy + Object are linked for the
# objcopy-as-a-library embedding). Unlike the other tools it links
# clangDriver directly: argument classification is done with clang's own
# driver, never a hand-rolled filter.
# =============================================================================

set(LLVM_LINK_COMPONENTS
  ObjCopy
  Object
  Support
  TargetParser
  )

set(LIBS
  MLIREmitRustImportC
  MLIREmitRustProject
  MLIREmitRustDialect
  MLIRUBToEmitRust
  MLIRArithToEmitRust
  MLIRFuncToEmitRust
  MLIRSCFToEmitRust
  MLIRLowerContainers
  MLIRConvertToEmitRust
  MLIRControlFlowToSCF
  MLIRTransforms
  MLIRFuncDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRControlFlowDialect
  MLIRSCFDialect
  MLIRUBDialect
  MLIRBytecodeWriter
  MLIRPass
  MLIRIR
  MLIRSupport
  clangDriver
  clangFrontend
  clangBasic
  )

add_llvm_executable(emitrust-clang
  emitrust-clang.cpp
  )
llvm_update_compile_flags(emitrust-clang)
target_link_libraries(emitrust-clang PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-clang)
