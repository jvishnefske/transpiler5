#!/usr/bin/env python3
"""The BUILD oracle for an external probe.  The progress JSON measures IMPORT
only and is structurally blind to 'the emitted crate does not compile', so
every emitted crate gets a real `cargo build --release --offline`.  Target dirs
are deleted as we go: 1500 crates of build artifacts do not fit on this disk."""
import argparse, concurrent.futures as cf, json, os, shutil, subprocess, sys, collections, re

ap = argparse.ArgumentParser()
ap.add_argument("--crates", required=True)
ap.add_argument("-j", type=int, default=8)
ap.add_argument("--timeout", type=int, default=300)
ap.add_argument("--out", default="")
a = ap.parse_args()
crates = sorted(d.path for d in os.scandir(a.crates)
                if d.is_dir() and os.path.exists(os.path.join(d.path, "Cargo.toml")))
print(f"{len(crates)} crates", file=sys.stderr)

def run(c):
    try:
        p = subprocess.run(["cargo", "build", "--release", "--offline"], cwd=c,
                           capture_output=True, text=True, timeout=a.timeout)
        rc, err = p.returncode, p.stderr[-6000:]
    except subprocess.TimeoutExpired:
        rc, err = "timeout", ""
    shutil.rmtree(os.path.join(c, "target"), ignore_errors=True)
    return {"crate": os.path.basename(c), "rc": rc, "err": err}

res = []
with cf.ThreadPoolExecutor(max_workers=a.j) as ex:
    for i, r in enumerate(ex.map(run, crates), 1):
        res.append(r)
        if i % 100 == 0: print(f"  {i}/{len(crates)}", file=sys.stderr)
if a.out: json.dump(res, open(a.out, "w"), indent=1)
bad = [r for r in res if r["rc"] != 0]
print(f"cargo build: {len(res)-len(bad)}/{len(res)} crates build clean")
codes = collections.Counter()
for r in bad:
    for m in set(re.findall(r"error\[(E\d+)\]", r["err"])) or {"(no Ennnn)"}:
        codes[m] += 1
for k, n in codes.most_common(20): print(f"  {n:5d} {k}")
print("\nsample messages:")
seen = set()
for r in bad:
    m = re.search(r"^error(\[\w+\])?: .*$", r["err"], re.M)
    if m and m.group(0)[:80] not in seen:
        seen.add(m.group(0)[:80]); print(f"  {r['crate'][:40]:42s} {m.group(0)[:110]}")
    if len(seen) > 14: break
