#!/usr/bin/env python3
"""emitrust CC shim -- drive a real project's build through the transpiler.

`emitrust-cc` is not a C compiler: it emits a Rust crate, not an object file,
and it does not link. So it cannot be `CC` literally for a project whose build
compiles to `.o` and then links. This shim is the equivalent: presented as
`CC` to an unmodified CMake/Make/autotools build, it intercepts every genuine
per-TU compile (`... -c foo.c -o foo.o`), runs `emitrust-cc` with the SAME
`-I/-isystem/-D/-std` flags the build chose, records the transpile verdict for
that TU as one JSONL line, and then SHADOW-COMPILES the TU with the real clang
so the build proceeds normally. A full per-TU transpile report over the whole
project falls out as a side effect of an ordinary build, and because the flags
are the build's own, PARSE_FAIL is near zero by construction (the same property
that made Track 5's 289-unit run credible).

Everything that is not a single-C-source `-c` compile -- link steps, `--version`
/ feature probes, CMake/autoconf try-compile scratch TUs -- is passed straight
through to the real compiler untouched, so configuration and linking behave
exactly as the project expects.

Environment (all set by the Nix derivation):
  EMITRUST_SHIM_REAL_CC      real C compiler to shadow-build with (clang)
  EMITRUST_SHIM_EMITRUST_CC  the emitrust-cc binary
  EMITRUST_SHIM_LOG          JSONL report path (appended, one line per TU)
  EMITRUST_SHIM_CRATES       optional dir to keep emitted crates (else TMPDIR)
  EMITRUST_RESOURCE_DIR      clang resource dir, so emitrust's libclang finds
                             the SAME builtin headers as the shadow compile
"""
import hashlib
import json
import os
import subprocess
import sys
import time

# Real compiler to shadow-build with, so a REAL project build keeps progressing
# past each transpiled TU (make/cmake still get their .o and can link). Set it
# to "none" (or leave empty) for a pure file SWEEP, where there is no build to
# keep alive and emitrust-cc's own diagnostic is the only signal wanted.
REAL_CC = os.environ.get("EMITRUST_SHIM_REAL_CC", "none")
EMITRUST_CC = os.environ["EMITRUST_SHIM_EMITRUST_CC"]
LOG = os.environ["EMITRUST_SHIM_LOG"]
CRATES = os.environ.get("EMITRUST_SHIM_CRATES", "")

# CMake/autoconf emit throwaway probe TUs; their path always carries one of
# these markers. Transpiling them would pollute the per-TU tally with the
# build system's own feature checks, so they pass straight through.
PROBE_MARKERS = ("CMakeFiles/", "CMakeScratch", "CMakeTmp", "TryCompile",
                 "conftest", "cmake.check_")


def expand_response_files(argv):
    """Expand leading-@ response files (ninja/cmake use them on long links)."""
    out = []
    for a in argv:
        if a.startswith("@") and os.path.exists(a[1:]):
            with open(a[1:], encoding="utf-8", errors="replace") as fh:
                out.extend(fh.read().split())
        else:
            out.append(a)
    return out


def keep_flags(argv):
    """The subset of the build's flags that affect AST construction, in order:
    include paths, defines, the language standard, and target/feature `-f`/`-m`
    flags. Output/dependency/optimization flags are dropped."""
    keep = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-I", "-isystem", "-iquote", "-idirafter", "-include", "-D",
                 "-U", "-x"):
            if i + 1 < len(argv):
                keep += [a, argv[i + 1]]
            i += 2
            continue
        if a.startswith(("-I", "-D", "-U", "-isystem", "-std=", "-f", "-m",
                         "-nostdinc", "-include", "--target=", "--sysroot=",
                         "-march", "-mcpu", "-mthumb", "-arch")):
            keep.append(a)
        i += 1
    return keep


def real_compile(argv):
    """Shadow-build with the real compiler, or None in sweep mode (no shadow)."""
    if not REAL_CC or REAL_CC == "none":
        return None
    return subprocess.call([REAL_CC] + argv)


def main():
    argv = expand_response_files(sys.argv[1:])

    sources = [a for a in argv
               if a.endswith(".c") and not a.startswith("-") and os.path.exists(a)]
    compiling = "-c" in argv and len(sources) == 1

    if not compiling:
        # link / probe / --version / assemble / multi-or-zero source: passthrough
        sys.exit(real_compile(argv) or 0)

    src = sources[0]
    norm = os.path.abspath(src).replace("\\", "/")
    if any(m in norm for m in PROBE_MARKERS):
        sys.exit(real_compile(argv) or 0)

    flags = keep_flags(argv)
    name = hashlib.sha1(norm.encode()).hexdigest()[:12]
    crate_dir = CRATES or os.environ.get("TMPDIR", "/tmp")
    out = os.path.join(crate_dir, name + ".crate")

    # emitrust-cc is an LLVM-CommandLine tool: clang flags (`-std=`, `-isystem
    # <dir>`, `-D...`) are NOT its own options, they are passed through with the
    # repeatable `--extra-arg=<arg>`, appended to clang verbatim (equivalent to
    # the flags a --compdb entry would carry). One --extra-arg per kept token.
    extra = ["--extra-arg=" + f for f in flags]
    t0 = time.time()
    proc = subprocess.run(
        [EMITRUST_CC, "--emit=crate", src, "-o", out] + extra,
        capture_output=True, text=True)
    diag = ""
    for line in proc.stderr.splitlines():
        if "error:" in line:
            diag = line.strip()
            break

    # Shadow-compile with the real toolchain so the project build proceeds;
    # its return code also tells us the TU actually parses/compiles (a genuine
    # config failure would fail here, separating it from a transpile REJECT).
    real_rc = real_compile(argv)

    record = {
        "tu": norm,
        "outcome": "TRANSPILED" if proc.returncode == 0 else "REJECT",
        "emitrust_rc": proc.returncode,
        "real_cc_rc": real_rc,
        "blocker": diag,
        "ms": int((time.time() - t0) * 1000),
        "flags": flags,
    }
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")

    # In sweep mode (no shadow) exit 0 so a driving loop continues regardless of
    # this TU's verdict; in build mode return the real compiler's code so the
    # project build sees exactly what it would have without the shim.
    sys.exit(real_rc if real_rc is not None else 0)


if __name__ == "__main__":
    main()
