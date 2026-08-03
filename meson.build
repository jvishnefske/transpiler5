# Meson build for the EmitRust standalone MLIR dialect, parallel to the
# CMake build (which remains the canonical CI configuration). Instead of the
# per-component static archives CMake links, this build links the monolithic
# libMLIR / libclang-cpp / libLLVM dylibs from the nix dev shell; the object
# code is the same, so tool behavior is identical.
#
# Usage (inside `nix develop`, which provides MLIR_DIR / Clang_DIR /
# LIBCLANG_PATH and puts llvm-config, mlir-tblgen and lit on PATH):
#   meson setup build-meson
#   meson compile -C build-meson
#   meson test -C build-meson --verbose    # runs the full lit suite
#
# The PDLL showcase (CMake's EMITRUST_ENABLE_PDLL, default OFF) is not
# mirrored here; the C++ patterns are the shipping baseline.
# b_ndebug=if-release matches CMake's Release -DNDEBUG: clang/LLVM inline
# header assertions must be compiled out, or C++ inputs whose decls have
# non-identifier names abort the importer instead of being rejected.
project('emitrust', 'cpp',
  default_options: ['cpp_std=c++17', 'buildtype=release',
                    'b_ndebug=if-release'],
  meson_version: '>=1.1.0')

fs = import('fs')
cpp = meson.get_compiler('cpp')

# ---------------------------------------------------------------------------
# Toolchain discovery. The nix dev shell exports MLIR_DIR / Clang_DIR /
# LIBCLANG_PATH (see flake.nix); everything else is derived from them, the
# same way CMake's find_package(MLIR)/find_package(Clang) would.
# ---------------------------------------------------------------------------
mlir_cmake_dir = run_command('sh', '-c', 'printf %s "$MLIR_DIR"',
  check: true).stdout()
clang_cmake_dir = run_command('sh', '-c', 'printf %s "$Clang_DIR"',
  check: true).stdout()
libclang_dir = run_command('sh', '-c', 'printf %s "$LIBCLANG_PATH"',
  check: true).stdout()
if mlir_cmake_dir == '' or clang_cmake_dir == '' or libclang_dir == ''
  error('MLIR_DIR, Clang_DIR and LIBCLANG_PATH must be set; ' +
        'run meson inside `nix develop`.')
endif

# <mlir-dev>/lib/cmake/mlir -> <mlir-dev>; headers live in <mlir-dev>/include.
mlir_dev_prefix = fs.parent(fs.parent(fs.parent(mlir_cmake_dir)))
mlir_inc_dir = mlir_dev_prefix / 'include'
clang_dev_prefix = fs.parent(fs.parent(fs.parent(clang_cmake_dir)))
clang_inc_dir = clang_dev_prefix / 'include'

# The MLIR libraries live in a different store path than the dev output;
# MLIRConfig.cmake records it as MLIR_INSTALL_PREFIX.
mlir_prefix = run_command('sh', '-c',
  'sed -n \'s/^set(MLIR_INSTALL_PREFIX "\\(.*\\)")$/\\1/p\' ' +
  '"$MLIR_DIR/MLIRConfig.cmake"',
  check: true).stdout().strip()
if mlir_prefix == ''
  error('could not read MLIR_INSTALL_PREFIX from MLIRConfig.cmake')
endif

# nixpkgs ships only the versioned dylib name (libMLIR.so.21.1, no dev
# symlink), so resolve it by glob instead of find_library.
libmlir_path = run_command('sh', '-c',
  'ls "' + mlir_prefix + '/lib"/libMLIR.so* 2>/dev/null | sort | head -n1',
  check: true).stdout().strip()
if libmlir_path == ''
  error('no libMLIR.so* under ' + mlir_prefix + '/lib')
endif

llvm_dep = dependency('llvm', version: '>=21', method: 'config-tool',
  modules: ['core', 'support', 'object', 'binaryformat', 'targetparser'])

mlir_dep = declare_dependency(
  include_directories: include_directories(mlir_inc_dir),
  link_args: [libmlir_path])

clang_dep = declare_dependency(
  include_directories: include_directories(clang_inc_dir),
  dependencies: cpp.find_library('clang-cpp', dirs: [libclang_dir]))

# Flags matching HandleLLVMOptions for this LLVM (RTTI on, EH off).
emitrust_cpp_args = [
  '-fno-exceptions',
  '-funwind-tables',
  '-D_GNU_SOURCE',
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
]

# Adds both include/ (dialect headers) and <builddir>/include/ (TableGen
# output; custom_target outputs mirror the source tree).
emitrust_inc = include_directories('include')

mlir_tblgen = find_program('mlir-tblgen')

# Populates emitrust_tablegen with every generated-.inc custom target.
emitrust_tablegen = []
subdir('include/EmitRust')

# Ordering-only dependency that makes any consumer wait for the generated
# headers (the canonical Meson generated-headers pattern).
tblgen_dep = declare_dependency(sources: emitrust_tablegen)

subdir('lib')
subdir('tools')
subdir('test')
