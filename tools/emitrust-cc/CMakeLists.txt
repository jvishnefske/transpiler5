# =============================================================================
# tools/emitrust-cc/CMakeLists.txt
# Builds emitrust-cc, the end-to-end C-to-Rust transpiler driver: it imports
# a C file with MLIREmitRustImportC, runs the pinned lowering pipeline
# (mem2reg, canonicalize, lift-cf-to-scf, canonicalize, convert-to-emitrust),
# and emits Rust source or a complete cargo crate. The clang libraries stay
# encapsulated inside MLIREmitRustImportC; the tool links MLIR libraries only.
# =============================================================================

set(LLVM_LINK_COMPONENTS
  Support
  )

set(LIBS
  MLIREmitRustImportC
  MLIREmitRustDialect
  MLIREmitRustTargetRust
  MLIRUBToEmitRust
  MLIRArithToEmitRust
  MLIRFuncToEmitRust
  MLIRSCFToEmitRust
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
  )

add_llvm_executable(emitrust-cc
  emitrust-cc.cpp
  CrateEmitter.cpp
  )
llvm_update_compile_flags(emitrust-cc)
target_link_libraries(emitrust-cc PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-cc)
