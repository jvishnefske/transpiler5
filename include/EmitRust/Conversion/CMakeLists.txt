# TableGen target for the EmitRust conversion pass declarations. The
# -name EmitRustConversion group makes GEN_PASS_REGISTRATION emit
# mlir::emitrust::registerEmitRustConversionPasses().
set(LLVM_TARGET_DEFINITIONS Passes.td)
mlir_tablegen(Passes.h.inc -gen-pass-decls -name EmitRustConversion)
add_public_tablegen_target(MLIREmitRustConversionPassIncGen)
