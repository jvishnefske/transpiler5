# FR-52: the pass that turns the importer's external-requirement markers into
# one emitrust.trait_def plus a type parameter on their transitive callers.
add_mlir_conversion_library(MLIREmitRustLowerExternalRequirements
  LowerExternalRequirements.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRIR
  MLIRPass
  )
