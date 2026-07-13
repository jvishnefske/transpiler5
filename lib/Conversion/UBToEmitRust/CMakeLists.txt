# The UB-to-EmitRust conversion library: ub.poison to emitrust.constant.
add_mlir_conversion_library(MLIRUBToEmitRust
  UBToEmitRust.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRUBDialect
  MLIRPass
  MLIRTransformUtils
  )
