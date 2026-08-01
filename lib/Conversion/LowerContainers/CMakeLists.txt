# The backend-agnostic container-op lowering (FR-39 array node pool).
add_mlir_conversion_library(MLIRLowerContainers
  LowerContainers.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRPass
  MLIRTransformUtils
  )
