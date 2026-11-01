# Puts the emitrust_bx package on sys.path for the pytest suite (FR-250).
# The tool is a standalone directory, not an installed package, so the
# tests import it the same way emitrust-bx.py does.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
