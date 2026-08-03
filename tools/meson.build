# All five tools are defined in this one meson.build so their binaries land
# together in <builddir>/tools/, which the lit site config points at as the
# tool directory (lit's add_tool_substitutions wants a single dir).
#
# Every executable links the monolithic libMLIR dylib (via mlir_dep), so the
# static libMLIR* archive lists from the CMake tool targets — including
# emitrust-opt's all-dialects/all-conversions registration libs — collapse
# into that one dependency; clang_dep (libclang-cpp) is added only where the
# CMake target pulls in clang libraries.

tool_link_args = emitrust_cpp_args

emitrust_opt = executable('emitrust-opt',
  'emitrust-opt/emitrust-opt.cpp',
  include_directories: emitrust_inc,
  cpp_args: tool_link_args,
  link_with: [lib_convert_to_emitrust, lib_lower_containers,
              lib_lower_external_requirements, lib_actor_lift],
  dependencies: [llvm_dep, mlir_dep, tblgen_dep])

emitrust_translate = executable('emitrust-translate',
  'emitrust-translate/emitrust-translate.cpp',
  include_directories: emitrust_inc,
  cpp_args: tool_link_args,
  link_with: [lib_target_rust],
  dependencies: [llvm_dep, mlir_dep, tblgen_dep])

emitrust_import_c = executable('emitrust-import-c',
  'emitrust-import-c/emitrust-import-c.cpp',
  include_directories: emitrust_inc,
  cpp_args: tool_link_args,
  link_with: [lib_importc],
  dependencies: [llvm_dep, mlir_dep, clang_dep, tblgen_dep])

emitrust_cc = executable('emitrust-cc',
  'emitrust-cc/emitrust-cc.cpp',
  'emitrust-cc/ActorLiftPlan.cpp',
  'emitrust-cc/ActorPlan.cpp',
  'emitrust-cc/CrateEmitter.cpp',
  'emitrust-cc/LinkMerge.cpp',
  'emitrust-cc/Partition.cpp',
  'emitrust-cc/RatchetReport.cpp',
  'emitrust-cc/ProgressReport.cpp',
  include_directories: emitrust_inc,
  cpp_args: tool_link_args,
  link_with: [lib_project, lib_importc, lib_target_rust,
              lib_convert_to_emitrust, lib_lower_containers,
              lib_lower_external_requirements, lib_actor_lift],
  dependencies: [llvm_dep, mlir_dep, clang_dep, tblgen_dep])

emitrust_clang = executable('emitrust-clang',
  'emitrust-clang/emitrust-clang.cpp',
  include_directories: emitrust_inc,
  cpp_args: tool_link_args,
  link_with: [lib_project, lib_importc,
              lib_convert_to_emitrust, lib_lower_containers],
  dependencies: [llvm_dep, mlir_dep, clang_dep, tblgen_dep])

emitrust_tools = [emitrust_opt, emitrust_translate, emitrust_import_c,
                  emitrust_cc, emitrust_clang]
