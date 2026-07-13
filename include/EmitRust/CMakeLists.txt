# TableGen targets for the EmitRust dialect: operations, dialect, and types
# are generated from EmitRustOps.td; attributes and enums are generated from
# EmitRustAttributes.td.

# Generates EmitRustOps{,Types,Dialect}.{h,cpp}.inc and the
# MLIREmitRustOpsIncGen target.
add_mlir_dialect(EmitRustOps emitrust)

# Attribute definitions (opaque attribute, cmp_predicate enum attribute).
set(LLVM_TARGET_DEFINITIONS EmitRustAttributes.td)
mlir_tablegen(EmitRustAttributes.h.inc -gen-attrdef-decls -attrdefs-dialect=emitrust)
mlir_tablegen(EmitRustAttributes.cpp.inc -gen-attrdef-defs -attrdefs-dialect=emitrust)
add_public_tablegen_target(MLIREmitRustAttributesIncGen)

# Enum definitions (CmpPredicate).
set(LLVM_TARGET_DEFINITIONS EmitRustAttributes.td)
mlir_tablegen(EmitRustEnums.h.inc -gen-enum-decls)
mlir_tablegen(EmitRustEnums.cpp.inc -gen-enum-defs)
add_public_tablegen_target(MLIREmitRustEnumsIncGen)

add_subdirectory(Conversion)
