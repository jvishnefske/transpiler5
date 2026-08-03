# Conversion pass declarations. The -name EmitRustConversion group makes
# GEN_PASS_REGISTRATION emit registerEmitRustConversionPasses(), same as the
# CMake MLIREmitRustConversionPassIncGen target.
emitrust_tablegen += custom_target('Passes.h.inc',
  input: 'Passes.td',
  output: 'Passes.h.inc',
  depfile: 'Passes.h.inc.d',
  command: [mlir_tblgen, '-gen-pass-decls', '-name', 'EmitRustConversion',
            '-I', meson.current_source_dir(),
            '-I', meson.project_source_root() / 'include',
            '-I', mlir_inc_dir,
            '@INPUT@', '-o', '@OUTPUT@', '-d', '@DEPFILE@'])
