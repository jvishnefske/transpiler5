# Implementation libraries: the EmitRust dialect, the core-MLIR-to-EmitRust
# conversion passes, the C importer, the pure-AST project analyses (FR-40's
# item graph), and the Rust emitter. Project comes after ImportC: it links it
# for the shared clang-driving shell.
add_subdirectory(EmitRust)
add_subdirectory(Conversion)
add_subdirectory(ImportC)
add_subdirectory(Project)
add_subdirectory(Target)
