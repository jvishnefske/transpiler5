#!/usr/bin/env python3
"""Timing-only re-run (no cargo) for E3/E6, best-of-N, records load average."""
import json, os, re, shutil, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from drive import projects, CC, WORK, SCRATCH, run
import synth

N = int(sys.argv[1]) if len(sys.argv) > 1 else 5
CORPORA = {"cpp-realworld", "c-realworld"}

ps = [p for p in projects() if p["corpus"] in CORPORA] + synth.SPECS

rows = []
for p in ps:
    wd = WORK / ("t_" + p["corpus"] + "_" + p["name"].replace("/", "_"))
    r = dict(corpus=p["corpus"], name=p["name"], tus=p["tus"])

    def timed(extra, outdir):
        best = None
        so = se = ""
        for _ in range(N):
            if outdir and Path(outdir).exists():
                shutil.rmtree(outdir)
            a = [str(CC)] + extra + p["inputs"]
            a += ["-o", str(outdir) if outdir else "-"]
            rc, so, se, ms = run(a, p["cwd"])
            best = ms if best is None else min(best, ms)
        return round(best, 1), so, se

    r["graph_ms"], so, _ = timed(["--emit=item-graph"], None)
    r["nodes"] = sum(1 for l in so.splitlines() if l.startswith("node "))
    r["edges"] = sum(1 for l in so.splitlines() if l.startswith("edge "))
    r["coloring_ms"], _, _ = timed(["--emit=coloring"], None)
    r["incremental_ms"], _, _ = timed(["--emit=crate", "--incremental"], wd)
    r["search_ms"], _, se = timed(
        ["--emit=crate", "--incremental", "--search", "--search-trace=-"], wd)
    m = re.search(r"^summary probes=(\d+)", se, re.M)
    r["probes"] = int(m.group(1)) if m else ""
    r["load"] = os.getloadavg()[0]
    shutil.rmtree(wd, ignore_errors=True)
    rows.append(r)
    print(r, flush=True)

(SCRATCH / "v3_raw_timing.json").write_text(json.dumps(rows, indent=1))
