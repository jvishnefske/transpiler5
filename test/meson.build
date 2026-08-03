# Registers the lit regression suite as a Meson test: `meson test` runs the
# same check-emitrust suite as `ninja -C build check-emitrust`, driven by the
# shared lit.cfg.py through a Meson-generated site config.

lit = find_program('lit', 'llvm-lit', required: false)
python3 = find_program('python3')

lit_cfg = configuration_data()
lit_cfg.set('LLVM_TOOLS_DIR', llvm_dep.get_variable(configtool: 'bindir'))
lit_cfg.set('LLVM_SHLIB_DIR', llvm_dep.get_variable(configtool: 'libdir'))
lit_cfg.set('PYTHON_EXECUTABLE', python3.full_path())
lit_cfg.set('EMITRUST_SRC_ROOT', meson.project_source_root())
lit_cfg.set('EMITRUST_OBJ_ROOT', meson.project_build_root())
lit_cfg.set('EMITRUST_TOOLS_DIR', meson.project_build_root() / 'tools')

configure_file(
  input: 'lit.site.cfg.py.meson.in',
  output: 'lit.site.cfg.py',
  configuration: lit_cfg)

if lit.found()
  # timeout: 0 disables Meson's per-test cap; lit.cfg.py applies its own
  # 600s per-test wall clock (the loop-miscompile hang detector).
  test('check-emitrust', lit,
    args: ['-sv', meson.current_build_dir()],
    depends: emitrust_tools,
    timeout: 0,
    is_parallel: false)
else
  warning('lit not found on PATH; `meson test` has no check-emitrust suite. '
          + 'Run meson inside `nix develop`.')
endif
