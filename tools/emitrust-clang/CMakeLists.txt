# =============================================================================
# tools/emitrust-clang/CMakeLists.txt
# Builds emitrust-clang, the FR-56 compiler-shim front end: a drop-in CC that
# delegates every invocation to the real clang (so builds, probes, and links
# behave identically) and side-emits one <object>.emitrust.mlir per compiled
# C translation unit. Unlike the other tools it links clangDriver directly:
# argument classification is done with clang's own driver, never a
# hand-rolled filter.
# =============================================================================

set(LLVM_LINK_COMPONENTS
  Support
  TargetParser
  )

set(LIBS
  MLIREmitRustImportC
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
  MLIRPass
  MLIRIR
  MLIRSupport
  clangDriver
  clangBasic
  )

add_llvm_executable(emitrust-clang
  emitrust-clang.cpp
  )
llvm_update_compile_flags(emitrust-clang)
target_link_libraries(emitrust-clang PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-clang)
