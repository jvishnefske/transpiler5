# =============================================================================
# lib/Target/Rust/CMakeLists.txt
# Builds MLIREmitRustTargetRust, the EmitRust-to-Rust translation library.
# TableGen dependencies are inherited by linking MLIREmitRustDialect.
# =============================================================================

add_mlir_translation_library(MLIREmitRustTargetRust
  TranslateToRust.cpp
  TranslateRegistration.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Target

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRIR
  MLIRSupport
  MLIRTranslateLib
  )
