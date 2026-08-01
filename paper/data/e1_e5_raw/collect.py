#!/usr/bin/env python3
"""E1/E5 collector: two pure-analysis invocations per project.

Reimplementation of the (unarchived) collect.py described in
paper-data/e1_e5_README.md, to the same corpus definitions, the same two
invocations and the same skip rule.

  1. emitrust-cc --emit=coloring -o - <inputs>          -> predicted colour
  2. emitrust-cc --emit=crate --incremental -o D <inputs>
       -> D/emitrust-progress.json -> actual per-item status

No cargo build, no --search.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from runline import endtoend_projects

ENDTOEND_SKIPPED = []

ROOT = Path("/home/j/src/transpiler5/.claude/worktrees/agent-a4ffe4dae7c4ea8b1")
CC = ROOT / "build/bin/emitrust-cc"
SCRATCH = Path("/tmp/claude-1000/-home-j-src-transpiler5/"
               "d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
WORK = SCRATCH / "v3work_e1"
TIMEOUT = 120

ITEM_RE = re.compile(r"^item (\S+) (.*)$")


def projects():
    ps = []
    for d in sorted((ROOT / "test/RealWorld/Cpp/Inputs").iterdir()):
        if d.is_dir() and (d / "compile_commands.json").exists():
            ps.append(dict(corpus="cpp-realworld", name=d.name, cwd=str(d),
                           inputs=["--compdb", "."]))
    base = ROOT / "test/RealWorld/Inputs"
    for e in sorted(base.iterdir()):
        if e.is_dir():
            srcs = sorted(str(p) for p in e.glob("*.c"))
            ps.append(dict(corpus="c-realworld", name=e.name, cwd=str(base),
                           inputs=srcs + ["-I" + str(e)]))
        elif e.suffix == ".c":
            ps.append(dict(corpus="c-realworld", name=e.stem, cwd=str(base),
                           inputs=[str(e)]))
    # EndToEnd: input set derived from each test's own lit RUN line (see
    # ../runline.py).  v2 fed every case only %s, which broke the multi-TU
    # tests by hiding the translation unit that defines their externals.
    e2e_ps, e2e_skipped = endtoend_projects(ROOT)
    for p in e2e_ps:
        ps.append(dict(corpus=p["corpus"], name=p["name"], cwd=p["cwd"],
                       inputs=list(p["inputs"])))
    ENDTOEND_SKIPPED.clear()
    ENDTOEND_SKIPPED.extend(e2e_skipped)
    cts = ROOT / "third_party/c-testsuite/tests/single-exec"
    for p in sorted(cts.glob("*.c")):
        ps.append(dict(corpus="c-testsuite", name=p.name, cwd=str(cts),
                       inputs=[str(p)]))
    return ps


def run(args, cwd):
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                           timeout=TIMEOUT)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "TIMEOUT"


def parse_coloring(text):
    """-> {symbol: {kind, color, reason}} plus the tally line."""
    items, tally = {}, {}
    for line in text.splitlines():
        m = ITEM_RE.match(line.strip())
        if m:
            sym, rest = m.group(1), m.group(2)
            toks = dict(t.split("=", 1) for t in rest.split(" ") if "=" in t)
            items[sym] = dict(kind=toks.get("kind", ""),
                              color=toks.get("color", ""),
                              reason=toks.get("reason", ""),
                              construct=toks.get("construct", ""),
                              chain=toks.get("chain", ""))
        elif line.startswith("tally "):
            tally = dict(t.split("=", 1) for t in line.split(" ")[1:]
                         if "=" in t)
    return items, tally


def measure(p):
    key = ("%s__%s" % (p["corpus"], p["name"])).replace("/", "_")
    wd = WORK / key
    if wd.exists():
        shutil.rmtree(wd)
    wd.mkdir(parents=True)
    rec = dict(corpus=p["corpus"], name=p["name"])

    args = [str(CC), "--emit=coloring", "-o", "-"] + p["inputs"]
    crc, cso, cse = run(args, p["cwd"])
    citems, tally = parse_coloring(cso)
    rec["coloring_rc"] = crc
    rec["coloring_items"] = citems
    rec["coloring_tally"] = tally
    rec["coloring_stderr_head"] = cse[:400]

    cdir = wd / "crate"
    args = ([str(CC), "--emit=crate", "--incremental"] + p["inputs"]
            + ["-o", str(cdir)])
    irc, iso, ise = run(args, p["cwd"])
    rec["incr_rc"] = irc
    rec["incr_stderr_head"] = ise[:400]
    pj = cdir / "emitrust-progress.json"
    rec["progress"] = None
    if pj.exists():
        try:
            rec["progress"] = json.loads(pj.read_text())
        except ValueError as exc:
            rec["progress_parse_error"] = str(exc)
    # crate text, for the `missing` audit
    rec["crate_kind"] = ""
    rec["crate_text"] = ""
    for stem in ("lib.rs", "main.rs"):
        f = cdir / "src" / stem
        if f.exists():
            rec["crate_kind"] = "lib" if stem == "lib.rs" else "bin"
            rec["crate_text"] = f.read_text(errors="replace")
            break
    shutil.rmtree(wd, ignore_errors=True)
    return rec


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    ps = projects()
    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=12) as ex:
        out = list(ex.map(measure, ps))
    print("%d projects in %.1f s" % (len(out), time.perf_counter() - t0))
    dest = SCRATCH / "v3_raw_e1e5.json"
    dest.write_text(json.dumps(out, indent=1))
    print("wrote", dest)


if __name__ == "__main__":
    main()
