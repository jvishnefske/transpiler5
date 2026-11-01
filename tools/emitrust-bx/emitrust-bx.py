#!/usr/bin/env python3
"""emitrust-bx entry point (FR-250).

A bijective path between a canonical C++17 subset and a canonical safe-Rust
subset through one typed Kernel IR. This file only puts the package on
sys.path and hands off to emitrust_bx.cli.main(); it shares no code with
lib/ImportC.
"""
import os
import sys

sys.dont_write_bytecode = True  # never dirty the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from emitrust_bx.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
