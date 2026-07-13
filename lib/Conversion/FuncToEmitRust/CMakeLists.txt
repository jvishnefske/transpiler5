# The Func-to-EmitRust conversion library: func.func/return/call to
# emitrust.func/return/call_opaque.
add_mlir_conversion_library(MLIRFuncToEmitRust
  FuncToEmitRust.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRFuncDialect
  MLIRPass
  MLIRTransformUtils
  )
