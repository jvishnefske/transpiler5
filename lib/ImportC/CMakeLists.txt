# The C importer library: clang-LibTooling-based translation from a C11
# subset to the hybrid core+EmitRust MLIR representation. The clang
# dependencies stay PRIVATE so that no other EmitRust library or tool picks
# them up transitively.
add_mlir_library(MLIREmitRustImportC
  ImportC.cpp
  ClangProjectParser.cpp
  ImportCTypes.cpp
  ImportCAggregates.cpp
  ImportCGlobals.cpp
  ImportCFunctions.cpp
  ImportCStatements.cpp
  ImportCHosted.cpp
  ImportCExpressions.cpp
  ImportCPlanning.cpp
  ImportCRecovery.cpp
  RejectionLedger.cpp

  ADDITIONAL_HEADER_DIRS
  ${PROJECT_SOURCE_DIR}/include/EmitRust

  DEPENDS
  MLIREmitRustOpsIncGen
  MLIREmitRustAttributesIncGen
  MLIREmitRustEnumsIncGen

  LINK_LIBS PUBLIC
  MLIREmitRustDialect
  MLIRFuncDialect
  MLIRArithDialect
  MLIRMemRefDialect
  MLIRControlFlowDialect
  MLIRIR
  MLIRSupport
  PRIVATE
  clangTooling
  clangFrontend
  clangSerialization
  clangAST
  clangLex
  clangBasic
  )

# Discover clang's builtin-header resource dir at configure time so the
# importer can pass `-resource-dir` and resolve system headers such as
# <stdint.h>. This is baked into the library as the EMITRUST_CLANG_RESOURCE_DIR
# compile define; the importer also honors the EMITRUST_RESOURCE_DIR
# environment variable at run time, which takes precedence. If clang is not
# found the define is simply omitted (the build never breaks) and system
# headers are only available when EMITRUST_RESOURCE_DIR is set.
find_program(EMITRUST_CLANG_EXECUTABLE NAMES clang)
if(EMITRUST_CLANG_EXECUTABLE)
  execute_process(
    COMMAND ${EMITRUST_CLANG_EXECUTABLE} -print-resource-dir
    OUTPUT_VARIABLE EMITRUST_CLANG_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE EMITRUST_CLANG_RESOURCE_RESULT)
  if(EMITRUST_CLANG_RESOURCE_RESULT EQUAL 0 AND EMITRUST_CLANG_RESOURCE_DIR)
    target_compile_definitions(MLIREmitRustImportC PRIVATE
      "EMITRUST_CLANG_RESOURCE_DIR=\"${EMITRUST_CLANG_RESOURCE_DIR}\"")
    message(STATUS
      "Using clang resource dir: ${EMITRUST_CLANG_RESOURCE_DIR}")
  endif()
endif()

# Same trick for Darwin's SDK sysroot: FixedCompilationDatabase/ClangTool
# invoke clang's frontend directly, never running clang::driver::Driver's
# SDK auto-detection, so <stdio.h> and friends are invisible without an
# explicit -isysroot. Reading $SDKROOT/shelling out to xcrun at RUN time
# (inside the actual importer) is unreliable under lit, which sandboxes the
# test environment and doesn't forward SDKROOT; probing here, once, inside
# the real configure-time environment and baking the answer in as
# EMITRUST_CLANG_SYSROOT sidesteps that. EMITRUST_SYSROOT still overrides at
# run time. $SDKROOT from the configuring shell (e.g. the nix dev shell) is
# tried first; `xcrun --show-sdk-path` covers a plain Xcode/Command Line
# Tools setup with no SDKROOT exported.
if(APPLE)
  set(EMITRUST_CLANG_SYSROOT "$ENV{SDKROOT}")
  if(NOT EMITRUST_CLANG_SYSROOT)
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE EMITRUST_CLANG_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE EMITRUST_CLANG_SYSROOT_RESULT
      ERROR_QUIET)
    if(NOT EMITRUST_CLANG_SYSROOT_RESULT EQUAL 0)
      set(EMITRUST_CLANG_SYSROOT "")
    endif()
  endif()
  if(EMITRUST_CLANG_SYSROOT)
    target_compile_definitions(MLIREmitRustImportC PRIVATE
      "EMITRUST_CLANG_SYSROOT=\"${EMITRUST_CLANG_SYSROOT}\"")
    message(STATUS "Using Darwin SDK sysroot: ${EMITRUST_CLANG_SYSROOT}")
  endif()

  # Same trick again for libc++'s headers (<vector>, <string>, ...): the nix
  # clang wrapper injects this path into every `clang`/`clang++` invocation
  # unconditionally (it is baked into the wrapper itself, not sourced from
  # $SDKROOT or any buildInputs-derived env var), but FixedCompilationDatabase/
  # ClangTool never run that wrapper script, so C++ imports fail on the
  # first standard header without it. Discovered by asking the wrapped
  # compiler what IT would search (`-v` on an empty translation unit) rather
  # than hardcoding a nix store path, so this survives a store path change.
  if(EMITRUST_CLANG_EXECUTABLE)
    # Search only within the printed "#include <...> search starts here"
    # block, and require "libcxx" in the path: an unfiltered grep matches
    # clang's own unresolved default entry first ("<bin>/../include/c++/v1",
    # printed even though it doesn't exist), not the wrapper's real,
    # absolute, nix-store-rooted injection.
    execute_process(
      COMMAND bash -c "echo | '${EMITRUST_CLANG_EXECUTABLE}++' -E -x c++ - -v 2>&1 | sed -n '/search starts here/,/End of search list/p' | grep -o '/[^ ]*libcxx[^ ]*include/c++/v1' | head -n1"
      OUTPUT_VARIABLE EMITRUST_CLANG_LIBCXX_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EMITRUST_CLANG_LIBCXX_DIR)
      target_compile_definitions(MLIREmitRustImportC PRIVATE
        "EMITRUST_CLANG_LIBCXX_DIR=\"${EMITRUST_CLANG_LIBCXX_DIR}\"")
      message(STATUS "Using Darwin libc++ include dir: ${EMITRUST_CLANG_LIBCXX_DIR}")
    endif()
  endif()
endif()
