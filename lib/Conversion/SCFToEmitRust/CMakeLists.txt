# The SCF-to-EmitRust conversion library: scf.if/while/for to
# emitrust.if/loop/for with mutable let bindings for the SSA results.
add_mlir_conversion_library(MLIRSCFToEmitRust
  SCFToEmitRust.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRSCFDialect
  MLIRPass
  MLIRTransformUtils
  )
