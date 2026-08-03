# FR-62 slice 5b: the module pass that rewrites lifted actors selected by
# the driver's emitrust.actor_thread attribute into the threaded-runtime
# driver shape (accessor synthesis, handle retype + spawn construction,
# shutdown insertion, actor_runtime anchors).
add_mlir_conversion_library(MLIREmitRustActorThread
  ActorThread.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust/Conversion

  DEPENDS
  MLIREmitRustConversionPassIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRIR
  MLIRPass
  )
