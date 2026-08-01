#!/usr/bin/env python3
"""E4 longitudinal harness: run ONE emitrust-cc build against the FIXED corpus.

Held constant across every commit under measurement:
  * the corpus (extracted once from the newest commit, bc0be94),
  * the discovery rules (copied from that commit's run_realworld.py),
  * the crate build step: `cargo build --release --offline` run by THIS script,
    never by the tool's own --build flag (whose semantics could drift).

Usage: e4_measure.py --tool <emitrust-cc> --corpus-root <dir> --workdir <dir>
                     --mode plain|incremental --out <json>
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys

TRANSPILE_TIMEOUT = 600
CARGO_TIMEOUT = 600


def run(cmd, timeout, cwd=None):
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), \
            p.stderr.decode("utf-8", "replace"), False
    except subprocess.TimeoutExpired:
        return -1, "", "", True
    except OSError as exc:
        return -1, "", str(exc), False


def discover_c(corpus_dir):
    progs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if os.path.isfile(path) and entry.endswith(".c"):
            progs.append((entry[:-2], [path], []))
        elif os.path.isdir(path):
            srcs = sorted(os.path.join(path, f) for f in os.listdir(path)
                          if f.endswith(".c"))
            if srcs:
                progs.append((entry, srcs, []))
    return progs


def _argv(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def _includes(argv, base):
    dirs, i = [], 0
    while i < len(argv):
        a, v = argv[i], None
        if a == "-I" and i + 1 < len(argv):
            v = argv[i + 1]
            i += 1
        elif a.startswith("-I") and len(a) > 2:
            v = a[2:]
        if v is not None:
            dirs.append(os.path.normpath(os.path.join(base, v)))
        i += 1
    return dirs


def discover_cpp(corpus_dir):
    progs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        db = os.path.join(path, "compile_commands.json")
        if not os.path.isdir(path) or not os.path.isfile(db):
            continue
        with open(db, encoding="utf-8") as h:
            entries = json.load(h)
        sources, incs = [], []
        for e in entries:
            d = e.get("directory", ".")
            if not os.path.isabs(d):
                d = os.path.join(path, d)
            d = os.path.normpath(d)
            s = e["file"]
            if not os.path.isabs(s):
                s = os.path.join(d, s)
            s = os.path.normpath(s)
            if s not in sources:
                sources.append(s)
            for inc in _includes(_argv(e), d):
                if inc not in incs:
                    incs.append(inc)
        progs.append((entry, sorted(sources), incs))
    return progs


def measure(tool, corpus, name, sources, incs, workdir, mode):
    crate = os.path.join(workdir, "%s.%s.%s" % (corpus, name, mode))
    if os.path.isdir(crate):
        shutil.rmtree(crate)
    cmd = [tool, "--emit=crate"]
    if mode == "incremental":
        cmd.append("--incremental")
    cmd += list(sources) + ["-I" + d for d in incs] + ["-o", crate]
    rc, out, err, to = run(cmd, TRANSPILE_TIMEOUT)
    rec = {
        "corpus": corpus, "project": name, "mode": mode,
        "tool_rc": rc, "tool_timeout": to,
        "tool_stderr_head": err[:800],
        "crate_emitted": 0, "crate_builds": 0,
        "graph_items": None, "ported": None,
    }
    manifest = os.path.join(crate, "Cargo.toml")
    rec["crate_emitted"] = 1 if (rc == 0 and os.path.isfile(manifest)) else 0

    # FR-51: which kind of crate came out.  Raw-JSON only -- the CSV schema is
    # frozen for comparability with the prior run.  A `lib` crate compiles but
    # has no entry point, so it carries no differential evidence.
    rec["crate_kind"] = ""
    if rec["crate_emitted"]:
        if os.path.isfile(os.path.join(crate, "src", "lib.rs")):
            rec["crate_kind"] = "lib"
        elif os.path.isfile(os.path.join(crate, "src", "main.rs")):
            rec["crate_kind"] = "bin"

    prog = os.path.join(crate, "emitrust-progress.json")
    if os.path.isfile(prog):
        try:
            with open(prog, encoding="utf-8") as h:
                report = json.load(h)
            t = report.get("totals", {})
            rec["graph_items"] = t.get("graph_items")
            rec["ported"] = t.get("ported")
        except (OSError, ValueError):
            pass

    if rec["crate_emitted"]:
        brc, _, berr, bto = run(
            ["cargo", "build", "--release", "--offline",
             "--manifest-path", manifest],
            CARGO_TIMEOUT)
        rec["crate_builds"] = 1 if (brc == 0 and not bto) else 0
        if not rec["crate_builds"]:
            rec["cargo_stderr_head"] = berr[:800]
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--corpus-root", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--mode", required=True, choices=["plain", "incremental"])
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.makedirs(a.workdir, exist_ok=True)
    c_dir = os.path.join(a.corpus_root, "test/RealWorld/Inputs")
    cpp_dir = os.path.join(a.corpus_root, "test/RealWorld/Cpp/Inputs")

    records = []
    for name, srcs, incs in discover_c(c_dir):
        records.append(measure(a.tool, "c", name, srcs, incs, a.workdir, a.mode))
        print("c/%s: %s" % (name, records[-1]["crate_emitted"]), flush=True)
    for name, srcs, incs in discover_cpp(cpp_dir):
        records.append(measure(a.tool, "cpp", name, srcs, incs, a.workdir, a.mode))
        print("cpp/%s: %s" % (name, records[-1]["crate_emitted"]), flush=True)

    with open(a.out, "w", encoding="utf-8") as h:
        json.dump(records, h, indent=1)
    print("wrote %s (%d records)" % (a.out, len(records)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
