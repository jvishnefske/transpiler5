#!/usr/bin/env python3
"""Per-unit import measurement of a third-party C project (Track 5 external
probe loop).  Pins nothing; it MEASURES.  For each translation unit in a
compile_commands.json it runs `emitrust-cc --emit=crate --incremental` and
aggregates the per-crate emitrust-progress.json, ranking blockers by FUNCTION
items (the all-kinds count is dominated by records re-imported per TU)."""
import argparse, concurrent.futures as cf, json, os, subprocess, sys, collections, pathlib

ap = argparse.ArgumentParser()
ap.add_argument("--compdb", required=True)
ap.add_argument("--tool", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--prefix", default="", help="only units whose path contains this")
ap.add_argument("-j", type=int, default=os.cpu_count() or 8)
ap.add_argument("--timeout", type=int, default=300)
ap.add_argument("--limit", type=int, default=0)
ap.add_argument("--root", default="", help="strip this prefix when naming crates")
a = ap.parse_args()

db = json.load(open(os.path.join(a.compdb, "compile_commands.json")))
seen, units = set(), []
for e in db:
    f = os.path.normpath(os.path.join(e["directory"], e["file"]))
    if not f.endswith(".c") or f in seen or not os.path.exists(f):
        continue
    if a.prefix and a.prefix not in f:
        continue
    seen.add(f)
    units.append(f)
units.sort()
if a.limit:
    units = units[: a.limit]
os.makedirs(a.out, exist_ok=True)
print(f"{len(units)} units", file=sys.stderr)

def run(f):
    rel = os.path.relpath(f, a.root) if a.root else f
    name = rel.removesuffix(".c").replace("/", "_").replace("..", "").strip("_")
    crate = os.path.join(a.out, name)
    cmd = [a.tool, "--compdb", a.compdb, "--emit=crate", "--crate-type=lib",
           "--incremental", f, "-o", crate]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout)
        rc, err = p.returncode, p.stderr[-4000:]
    except subprocess.TimeoutExpired:
        rc, err = "timeout", ""
    prog = os.path.join(crate, "emitrust-progress.json")
    j = json.load(open(prog)) if os.path.exists(prog) else None
    return {"file": f, "crate": crate, "rc": rc, "stderr": err, "progress": j}

res = []
with cf.ThreadPoolExecutor(max_workers=a.j) as ex:
    for i, r in enumerate(ex.map(run, units), 1):
        res.append(r)
        if i % 25 == 0:
            print(f"  {i}/{len(units)}", file=sys.stderr)
json.dump(res, open(os.path.join(a.out, "_measure.json"), "w"), indent=1)

# ---- aggregate -----------------------------------------------------------
ok = [r for r in res if r["progress"]]
tot_all = tot_fn = por_all = por_fn = 0
diag_fn = collections.Counter(); diag_all = collections.Counter()
for r in ok:
    j = r["progress"]
    items = j.get("items") or j.get("graph_items") or []
    if isinstance(items, dict):
        items = list(items.values())
    for it in items:
        kind = it.get("kind", "?")
        ported = it.get("status") == "ported" or it.get("ported") is True
        tot_all += 1; por_all += ported
        if kind == "function":
            tot_fn += 1; por_fn += ported
            if not ported:
                diag_fn[it.get("diagnostic") or it.get("blocker") or "?"] += 1
        if not ported:
            diag_all[it.get("diagnostic") or it.get("blocker") or "?"] += 1
print(f"\nunits: {len(ok)}/{len(res)} produced a crate")
print(f"items  all-kinds: {por_all}/{tot_all}")
print(f"items  FUNCTION : {por_fn}/{tot_fn}")
print("\ntop FUNCTION blockers:")
for d, n in diag_fn.most_common(20):
    print(f"  {n:6d}  {d[:110]}")
print("\ntop all-kind blockers:")
for d, n in diag_all.most_common(10):
    print(f"  {n:6d}  {d[:110]}")
fails = collections.Counter()
for r in res:
    if not r["progress"]:
        line = next((l for l in reversed(r["stderr"].splitlines()) if "error" in l.lower()), r["stderr"][-160:].replace("\n", " "))
        fails[line.split(": error: ")[-1][:110]] += 1
print(f"\nno-crate units: {sum(fails.values())}")
for d, n in fails.most_common(15):
    print(f"  {n:6d}  {d}")
