# Implementation libraries, mirroring the per-directory CMake targets: the
# EmitRust dialect, the conversion passes, the C importer, the project
# analyses, and the Rust emitter. Each static_library carries its CMake
# LINK_LIBS as link_with/dependencies so Meson propagates and topologically
# orders them at the executables.

common_deps = [llvm_dep, mlir_dep, tblgen_dep]

libemitrust_dialect = static_library('MLIREmitRustDialect',
  'EmitRust/EmitRustAttributes.cpp',
  'EmitRust/EmitRustDialect.cpp',
  'EmitRust/EmitRustOps.cpp',
  'EmitRust/EmitRustRangeInference.cpp',
  'EmitRust/EmitRustTypes.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  dependencies: common_deps)

lib_ub_to_emitrust = static_library('MLIRUBToEmitRust',
  'Conversion/UBToEmitRust/UBToEmitRust.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_arith_to_emitrust = static_library('MLIRArithToEmitRust',
  'Conversion/ArithToEmitRust/ArithToEmitRust.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_func_to_emitrust = static_library('MLIRFuncToEmitRust',
  'Conversion/FuncToEmitRust/FuncToEmitRust.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_scf_to_emitrust = static_library('MLIRSCFToEmitRust',
  'Conversion/SCFToEmitRust/SCFToEmitRust.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_lower_containers = static_library('MLIRLowerContainers',
  'Conversion/LowerContainers/LowerContainers.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_convert_to_emitrust = static_library('MLIRConvertToEmitRust',
  'Conversion/ConvertToEmitRust/ConvertToEmitRust.cpp',
  'Conversion/ConvertToEmitRust/RangeRefinementCheck.cpp',
  'Conversion/ConvertToEmitRust/ValueIdentityCheck.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [lib_ub_to_emitrust, lib_arith_to_emitrust,
              lib_func_to_emitrust, lib_scf_to_emitrust,
              libemitrust_dialect],
  dependencies: common_deps)

lib_lower_external_requirements = static_library(
  'MLIREmitRustLowerExternalRequirements',
  'Conversion/LowerExternalRequirements/LowerExternalRequirements.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_actor_lift = static_library('MLIREmitRustActorLift',
  'Conversion/ActorLift/ActorLift.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

lib_actor_thread = static_library('MLIREmitRustActorThread',
  'Conversion/ActorThread/ActorThread.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)

# Bake clang's builtin-header resource dir in as EMITRUST_CLANG_RESOURCE_DIR,
# same as the CMake configure-time probe; EMITRUST_RESOURCE_DIR still wins at
# run time, and a missing clang just omits the define.
clang_exe = find_program('clang', required: false)
importc_resource_args = []
if clang_exe.found()
  resource_probe = run_command(clang_exe, '-print-resource-dir', check: false)
  if resource_probe.returncode() == 0 and resource_probe.stdout().strip() != ''
    importc_resource_args = ['-DEMITRUST_CLANG_RESOURCE_DIR="'
                             + resource_probe.stdout().strip() + '"']
  endif
endif

# Same trick for Darwin's SDK sysroot (see the CMakeLists.txt probe next to
# this file for why it must be baked in at configure time rather than read
# from $SDKROOT/xcrun at run time: lit sandboxes the test environment and
# doesn't forward SDKROOT). $SDKROOT from the configuring shell (e.g. the
# nix dev shell) is tried first; `xcrun --show-sdk-path` covers a plain
# Xcode/Command Line Tools setup with no SDKROOT exported.
if host_machine.system() == 'darwin'
  sysroot_probe = run_command('sh', '-c',
    'printf %s "${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null)}"',
    check: false)
  emitrust_sysroot = sysroot_probe.stdout().strip()
  if sysroot_probe.returncode() == 0 and emitrust_sysroot != ''
    importc_resource_args += ['-DEMITRUST_CLANG_SYSROOT="' + emitrust_sysroot + '"']
  endif

  # Same trick again for libc++'s headers (see CMakeLists.txt's probe next
  # to this file for why: the nix clang wrapper injects this path
  # unconditionally, but FixedCompilationDatabase/ClangTool never run that
  # wrapper). Discovered by asking the wrapped compiler what it would
  # search, rather than hardcoding a nix store path.
  if clang_exe.found()
    # Search only within the printed "#include <...> search starts here"
    # block, and require "libcxx" in the path: an unfiltered grep matches
    # clang's own unresolved default entry first ("<bin>/../include/c++/v1",
    # printed even though it doesn't exist), not the wrapper's real,
    # absolute, nix-store-rooted injection.
    libcxx_probe = run_command('sh', '-c',
      'echo | "' + clang_exe.full_path() + '++" -E -x c++ - -v 2>&1 | '
      + 'sed -n "/search starts here/,/End of search list/p" | '
      + 'grep -o "/[^ ]*libcxx[^ ]*include/c++/v1" | head -n1',
      check: false)
    emitrust_libcxx_dir = libcxx_probe.stdout().strip()
    if libcxx_probe.returncode() == 0 and emitrust_libcxx_dir != ''
      importc_resource_args += ['-DEMITRUST_CLANG_LIBCXX_DIR="' + emitrust_libcxx_dir + '"']
    endif
  endif
endif

lib_importc = static_library('MLIREmitRustImportC',
  'ImportC/ImportC.cpp',
  'ImportC/ClangProjectParser.cpp',
  'ImportC/ImportCTypes.cpp',
  'ImportC/ImportCAggregates.cpp',
  'ImportC/ImportCGlobals.cpp',
  'ImportC/ImportCFunctions.cpp',
  'ImportC/ImportCStatements.cpp',
  'ImportC/ImportCHosted.cpp',
  'ImportC/ImportCExpressions.cpp',
  'ImportC/ImportCPlanning.cpp',
  'ImportC/ImportCRecovery.cpp',
  'ImportC/RejectionLedger.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args + importc_resource_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps + [clang_dep])

lib_project = static_library('MLIREmitRustProject',
  'Project/FrontierSearch.cpp',
  'Project/ItemColoring.cpp',
  'Project/ItemGraph.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [lib_importc],
  dependencies: common_deps + [clang_dep])

lib_target_rust = static_library('MLIREmitRustTargetRust',
  'Target/Rust/TranslateToRust.cpp',
  'Target/Rust/TranslateRegistration.cpp',
  include_directories: emitrust_inc,
  cpp_args: emitrust_cpp_args,
  link_with: [libemitrust_dialect],
  dependencies: common_deps)
