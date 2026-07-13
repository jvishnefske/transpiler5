# =============================================================================
# tools/emitrust-translate/CMakeLists.txt
# Builds emitrust-translate, the mlir-translate-style driver exposing the
# --mlir-to-rust translation. Mirrors the standalone example's translate tool.
# =============================================================================

set(LLVM_LINK_COMPONENTS
  Support
  )

set(LIBS
  MLIREmitRustTargetRust
  MLIRTranslateLib
  MLIRSupport
  MLIRIR
  )

add_llvm_executable(emitrust-translate emitrust-translate.cpp)
llvm_update_compile_flags(emitrust-translate)
target_link_libraries(emitrust-translate PRIVATE ${LIBS})

mlir_check_all_link_libraries(emitrust-translate)
