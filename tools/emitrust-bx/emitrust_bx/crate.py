"""--emit=crate: a self-contained cargo binary crate (FR-250).

Layout and argv contract match emitrust-cc so test/harness/ledger_runner.py
can drive either tool: `--emit=crate SRC -o DIR [--build]` writes
DIR/Cargo.toml and DIR/src/main.rs, and --build runs
`cargo build --release --offline --manifest-path DIR/Cargo.toml`, leaving
the binary at DIR/target/release/<name> where <name> is the source stem
sanitized exactly like emitrust-cc (ledger_runner.sanitize_crate_binary_name).
No dependencies; `[workspace]` keeps the crate out of any enclosing
workspace; `[profile.release]` is cargo's default.
"""

import os
import shutil
import subprocess


def sanitize_crate_binary_name(stem):
    """Mirror of test/harness/ledger_runner.py sanitize_crate_binary_name."""
    lowered = stem.lower()
    sanitized = "".join(
        ch if (ch.isascii() and (ch.isdigit() or "a" <= ch <= "z" or ch == "_")) else "_"
        for ch in lowered
    )
    if not sanitized:
        return "transpiled"
    if sanitized[0].isdigit():
        return "_" + sanitized
    return sanitized


def cargo_toml(name):
    return (
        "[package]\n"
        "name = \"%s\"\n"
        "version = \"0.1.0\"\n"
        "edition = \"2021\"\n"
        "\n"
        "[dependencies]\n"
        "\n"
        "[profile.release]\n"
        "\n"
        "[workspace]\n" % name
    )


def write_crate(out_dir, src_path, rust_source):
    stem = os.path.splitext(os.path.basename(src_path))[0]
    name = sanitize_crate_binary_name(stem)
    os.makedirs(os.path.join(out_dir, "src"), exist_ok=True)
    with open(os.path.join(out_dir, "Cargo.toml"), "w") as f:
        f.write(cargo_toml(name))
    with open(os.path.join(out_dir, "src", "main.rs"), "w") as f:
        f.write(rust_source)
    return name


def build_crate(out_dir):
    """Returns (ok, message)."""
    cargo = shutil.which("cargo")
    if cargo is None:
        return False, "cannot find 'cargo' on PATH"
    p = subprocess.run([cargo, "build", "--release", "--offline", "--manifest-path",
                        os.path.join(out_dir, "Cargo.toml")])
    if p.returncode != 0:
        return False, "cargo build failed with exit code %d" % p.returncode
    return True, ""
