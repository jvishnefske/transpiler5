#!/usr/bin/env python3
"""--max-search-nodes sweep for E3."""
import json, re, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from drive import projects, CC, WORK, SCRATCH, run, crate_ok
import synth

BOUNDS = [1, 2, 4, 8, 16]


def main():
    names = set(sys.argv[1:])
    allp = [p for p in projects() if p["corpus"] != "synthetic"] + synth.SPECS
    ps = [p for p in allp if p["name"] in names or
          f"{p['corpus']}/{p['name']}" in names]
    rows = []
    for p in ps:
        wd = WORK / ("sweep_" + p["name"].replace("/", "_"))
        for b in BOUNDS:
            best, se, rc = None, "", 0
            for _ in range(3):
                if wd.exists():
                    shutil.rmtree(wd)
                a = ([str(CC), "--emit=crate", "--incremental", "--search",
                      "--search-trace=-", f"--max-search-nodes={b}"]
                     + p["inputs"] + ["-o", str(wd)])
                rc, so, se, ms = run(a, p["cwd"])
                best = ms if best is None else min(best, ms)
            m = re.search(r"^summary probes=(\d+)", se, re.M)
            probes = int(m.group(1)) if m else ""
            ported = ""
            pj = wd / "emitrust-progress.json"
            if pj.exists():
                ported = json.loads(pj.read_text())["totals"]["ported"]
            rows.append(dict(project=f"{p['corpus']}/{p['name']}", max_nodes=b,
                             probes=probes, wall_ms=round(best, 1),
                             ported=ported, rc=rc))
            print(rows[-1], flush=True)
        shutil.rmtree(wd, ignore_errors=True)
    (SCRATCH / "v3_raw_sweep.json").write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
