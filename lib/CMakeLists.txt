# Implementation libraries: the EmitRust dialect, the core-MLIR-to-EmitRust
# conversion passes, the C importer, and the Rust emitter.
add_subdirectory(EmitRust)
add_subdirectory(Conversion)
add_subdirectory(ImportC)
add_subdirectory(Target)
