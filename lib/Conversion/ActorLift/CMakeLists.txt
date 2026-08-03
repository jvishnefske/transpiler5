# FR-62 slice 4: the module pass that lifts planned actor clusters into
# struct-owned state (struct_def + impl + receiver rewriting), driven purely
# by the discardable attributes the emitrust-cc driver attaches.
add_mlir_conversion_library(MLIREmitRustActorLift
  ActorLift.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRIR
  MLIRPass
  )
