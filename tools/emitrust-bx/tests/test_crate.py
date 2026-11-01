# Pins --emit=crate (FR-250): argv/layout compatibility with emitrust-cc so
# test/harness/ledger_runner.py can drive emitrust-bx unchanged (same
# binary-name sanitization), and the zero-`unsafe` rule for EVERYTHING the
# tool emits, including the bx helper module.
import os
import sys

from emitrust_bx import crate
from emitrust_bx import gen
from emitrust_bx import rs_print as RSP

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "test", "harness"))


def test_sanitize_matches_ledger_runner():
    import ledger_runner
    for stem in ["bx-scalars", "Foo.Bar", "9lives", "", "ünï", "a_b"]:
        assert crate.sanitize_crate_binary_name(stem) == ledger_runner.sanitize_crate_binary_name(stem)


def test_write_crate_layout(tmp_path):
    name = crate.write_crate(str(tmp_path / "out"), "/x/bx-demo.cpp", "fn main() {\n}\n")
    assert name == "bx_demo"
    toml = (tmp_path / "out" / "Cargo.toml").read_text()
    assert 'name = "bx_demo"' in toml and "[workspace]" in toml and "[dependencies]\n\n" in toml
    assert (tmp_path / "out" / "src" / "main.rs").read_text() == "fn main() {\n}\n"


def test_no_unsafe_in_generated_rust():
    for seed in range(5):
        text = RSP.rs_print(gen.gen_program(seed, 40))
        assert "bx::fmt_f" in text or "bx::chr" in text or seed  # helpers get exercised
        assert "unsafe" not in text
