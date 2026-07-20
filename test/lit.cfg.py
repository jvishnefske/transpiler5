# -*- Python -*-

import os
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

llvm_config.with_system_environment(["HOME", "INCLUDE", "LIB", "TMP", "TEMP"])

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
tools = ["emitrust-opt", "emitrust-translate", "emitrust-import-c", "emitrust-cc"]

llvm_config.add_tool_substitutions(tools, tool_dirs)

# The differential EndToEnd tests build the emitted crate with cargo; make
# them conditional on cargo being available on PATH.
if shutil.which("cargo"):
    config.available_features.add("cargo")
