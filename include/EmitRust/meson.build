# TableGen for the EmitRust dialect, mirroring the CMake add_mlir_dialect /
# mlir_tablegen calls: ops, types and dialect from EmitRustOps.td; attributes
# and enums from EmitRustAttributes.td. Outputs land in
# <builddir>/include/EmitRust/ so `#include "EmitRust/Foo.inc"` resolves via
# the build-side include/ directory.

tblgen_inc_args = [
  '-I', meson.current_source_dir(),
  '-I', meson.project_source_root() / 'include',
  '-I', mlir_inc_dir,
]

# [output, generator args, input .td]
tablegen_specs = [
  ['EmitRustOps.h.inc', ['-gen-op-decls'], 'EmitRustOps.td'],
  ['EmitRustOps.cpp.inc', ['-gen-op-defs'], 'EmitRustOps.td'],
  ['EmitRustOpsTypes.h.inc',
   ['-gen-typedef-decls', '-typedefs-dialect=emitrust'], 'EmitRustOps.td'],
  ['EmitRustOpsTypes.cpp.inc',
   ['-gen-typedef-defs', '-typedefs-dialect=emitrust'], 'EmitRustOps.td'],
  ['EmitRustOpsDialect.h.inc',
   ['-gen-dialect-decls', '-dialect=emitrust'], 'EmitRustOps.td'],
  ['EmitRustOpsDialect.cpp.inc',
   ['-gen-dialect-defs', '-dialect=emitrust'], 'EmitRustOps.td'],
  ['EmitRustAttributes.h.inc',
   ['-gen-attrdef-decls', '-attrdefs-dialect=emitrust'],
   'EmitRustAttributes.td'],
  ['EmitRustAttributes.cpp.inc',
   ['-gen-attrdef-defs', '-attrdefs-dialect=emitrust'],
   'EmitRustAttributes.td'],
  ['EmitRustEnums.h.inc', ['-gen-enum-decls'], 'EmitRustAttributes.td'],
  ['EmitRustEnums.cpp.inc', ['-gen-enum-defs'], 'EmitRustAttributes.td'],
]

foreach spec : tablegen_specs
  emitrust_tablegen += custom_target(spec[0],
    input: spec[2],
    output: spec[0],
    depfile: spec[0] + '.d',
    command: [mlir_tblgen, spec[1], tblgen_inc_args,
              '@INPUT@', '-o', '@OUTPUT@', '-d', '@DEPFILE@'])
endforeach

subdir('Conversion')
