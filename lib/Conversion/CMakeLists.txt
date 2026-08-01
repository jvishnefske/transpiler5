# Conversion passes lowering core MLIR dialects into EmitRust. Each
# subdirectory mirrors the upstream mlir/lib/Conversion layout and builds one
# conversion library; ConvertToEmitRust is the composite pass over all of
# them.
add_subdirectory(UBToEmitRust)
add_subdirectory(ArithToEmitRust)
add_subdirectory(FuncToEmitRust)
add_subdirectory(SCFToEmitRust)
add_subdirectory(LowerContainers)
add_subdirectory(ConvertToEmitRust)
add_subdirectory(LowerExternalRequirements)
