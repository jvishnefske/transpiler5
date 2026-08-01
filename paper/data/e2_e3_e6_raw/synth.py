#!/usr/bin/env python3
"""Synthetic test/Project cases with their correct multi-TU / -I arguments."""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import drive
from drive import ROOT, SCRATCH, WORK, OUT

P = ROOT / "test/Project"
I = P / "Inputs"

SPECS = [
    dict(corpus="synthetic", name="search-backtrack.cpp", cwd=str(P), tus=2,
         inputs=[str(P / "search-backtrack.cpp"),
                 str(I / "search-backtrack-other.cpp"), "-I", str(I)]),
    dict(corpus="synthetic", name="search-false-red.cpp", cwd=str(P), tus=1,
         inputs=[str(P / "search-false-red.cpp")]),
    dict(corpus="synthetic", name="search-red.c", cwd=str(P), tus=1,
         inputs=[str(P / "search-red.c")]),
    dict(corpus="synthetic", name="search-green.c", cwd=str(P), tus=1,
         inputs=[str(P / "search-green.c")]),
    dict(corpus="synthetic", name="search-false-green.c", cwd=str(P), tus=1,
         inputs=[str(P / "search-false-green.c")]),
    dict(corpus="synthetic", name="search-bound.c", cwd=str(P), tus=1,
         inputs=[str(P / "search-bound.c")]),
    dict(corpus="synthetic", name="search-determinism.c", cwd=str(P), tus=2,
         inputs=[str(P / "search-determinism.c"),
                 str(I / "search-determinism-other.c"), "-I", str(I)]),
    dict(corpus="synthetic", name="search-cli.c", cwd=str(P), tus=2,
         inputs=[str(P / "search-cli.c"), str(I / "search-cli-main.c")]),
]

if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    out = []
    for p in SPECS:
        r = drive.measure(p, timed_reps=3)
        out.append(r)
        print(p["name"], "strict=%d/%d" % (r["strict_ok"], r["strict_builds"]),
              "incr=%d/%d" % (r["incr_emitted"], r["incr_builds"]),
              "items=%s ported=%s probes=%s sported=%s improved=%s"
              % (r["graph_items"], r["ported"], r["probes"],
                 r["search_ported_total"], r["search_improved"]), flush=True)
    (SCRATCH / "v3_raw_synth.json").write_text(json.dumps(out, indent=1))
