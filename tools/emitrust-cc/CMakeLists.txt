# =============================================================================
# tools/emitrust-cc/CMakeLists.txt
# Builds emitrust-cc, the end-to-end C-to-Rust transpiler driver: it imports
# a C file with MLIREmitRustImportC, runs the pinned lowering pipeline
# (mem2reg, canonicalize, lift-cf-to-scf, canonicalize, convert-to-emitrust),
# and emits Rust source or a complete cargo crate. The clang libraries stay
# encapsulated inside MLIREmitRustImportC; the tool links MLIR libraries only.
# =============================================================================

# Object: FR-58 --link reads each link input as an llvm::object::ObjectFile
# to extract the `.emitrust` section the FR-56 shim embedded.
set(LLVM_LINK_COMPONENTS
  Object
  Support
  )

set(LIBS
  MLIREmitRustImportC
  MLIREmitRustProject
  MLIREmitRustDialect
  MLIREmitRustTargetRust
  MLIRUBToEmitRust
  MLIRArithToEmitRust
  MLIRFuncToEmitRust
  MLIRSCFToEmitRust
  MLIRLowerContainers
  MLIRConvertToEmitRust
  MLIREmitRustLowerExternalRequirements
  MLIRControlFlowToSCF
  MLIRTransforms
  MLIRFuncDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRControlFlowDialect
  MLIRSCFDialect
  MLIRUBDialect
  MLIRPass
  MLIRParser
  MLIRBytecodeReader
  MLIRIR
  MLIRSupport
  )

add_llvm_executable(emitrust-cc
  emitrust-cc.cpp
  CrateEmitter.cpp
  LinkMerge.cpp
  ProgressReport.cpp
  )
llvm_update_compile_flags(emitrust-cc)
target_link_libraries(emitrust-cc PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-cc)
