# -*- Python -*-

import os
import platform
import shutil

import lit.formats
import lit.util

from lit.llvm import llvm_config

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = "EMITRUST"

config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
# ".cpp" (W2.0) covers the C++-input subset tests under Import/Cpp and the
# clang++-native-leg differential test in EndToEnd.
config.suffixes = [".mlir", ".c", ".cpp"]

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.emitrust_obj_root, "test")

config.substitutions.append(("%PATH%", config.environment["PATH"]))
config.substitutions.append(("%shlibext", config.llvm_shlib_ext))

# The `.emitrust` payload section emitrust-clang embeds needs a
# "<segment>,<section>" spelling on Mach-O (llvm-objcopy rejects a flat
# ELF-style name there); tests that strip/dump it with llvm-objcopy directly
# use this substitution instead of hardcoding the ELF spelling. Matches
# emitRustSectionSpec in emitrust-clang.cpp.
if platform.system() == "Darwin":
    config.available_features.add("system-darwin")
config.substitutions.append((
    "%emitrust_section_spec",
    "__TEXT,__emitrust" if platform.system() == "Darwin" else ".emitrust",
))

llvm_config.with_system_environment(["HOME", "INCLUDE", "LIB", "TMP", "TEMP"])

# The EndToEnd --build tests have cargo invoke rustc, whose Darwin linker
# step resolves the SDK from $SDKROOT and otherwise shells out to
# `xcrun --show-sdk-path`, which fails inside the sandboxed test
# environment ("error: unable to find sdk: 'macosx'"). The nix dev shell
# exports a correct SDKROOT; forward it. (The IMPORTER deliberately does
# not rely on this — it bakes the SDK path in at configure time, see
# darwinSysrootArg — this is only for the rustc/cc subprocesses tests
# spawn.)
if platform.system() == "Darwin":
    llvm_config.with_system_environment(["SDKROOT"])

# use_default_substitutions provides FileCheck, not, count, %s, %t, etc.
llvm_config.use_default_substitutions()

# excludes: A list of directories/files to exclude from the testsuite.
config.excludes = [
    "Inputs",
    "CMakeLists.txt",
    "README.txt",
    "LICENSE.txt",
    "lit.cfg.py",
]

# Tweak the PATH to include the tool directories.
llvm_config.with_environment("PATH", config.emitrust_tools_dir, append_path=True)
llvm_config.with_environment("PATH", config.llvm_tools_dir, append_path=True)

tool_dirs = [config.emitrust_tools_dir, config.llvm_tools_dir]
tools = ["emitrust-opt", "emitrust-translate", "emitrust-import-c", "emitrust-cc",
         "emitrust-clang"]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# The differential EndToEnd tests build the emitted crate with cargo; make
# them conditional on cargo being available on PATH.
if shutil.which("cargo"):
    config.available_features.add("cargo")

# Per-test wall-clock cap. The EndToEnd tests RUN the emitted binary, so an
# emitter miscompile that drops a loop's exit-condition store presents as a
# NON-TERMINATING test, not a byte diff; without a cap it wedges the suite
# forever (observed 2026-08-03: a stale run had burned ~28 CPU-hours per
# spinning binary). 10 minutes is far above the slowest legitimate test
# (first-build cargo compiles included) and turns a hang into a loud
# TIMEOUT. Requires psutil in the lit python env; skip gracefully when the
# harness lacks it rather than failing every configuration.
if not lit_config.maxIndividualTestTime:
    try:
        import psutil  # noqa: F401 -- lit's timeout support requires it

        lit_config.maxIndividualTestTime = 600
    except ImportError:
        lit_config.warning("psutil unavailable: no per-test timeout")
