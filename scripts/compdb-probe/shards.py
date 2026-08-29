#!/usr/bin/env python3
"""FR-58 whole-program path for a third-party project: compile every object a
binary target links with the emitrust-clang shim (each .o carries its MLIR
shard), so `emitrust-cc --link --emit=crate` can merge them into ONE crate.
Measures what the link step does on a real 500-TU program, not a toy."""
import argparse, concurrent.futures as cf, json, os, shlex, subprocess, sys, collections

ap = argparse.ArgumentParser()
ap.add_argument("--compdb", required=True)
ap.add_argument("--objects", required=True, help="file with one ninja object path per line")
ap.add_argument("--clang", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("-j", type=int, default=os.cpu_count() or 8)
ap.add_argument("--timeout", type=int, default=300)
a = ap.parse_args()

db = {e["output"]: e for e in json.load(open(os.path.join(a.compdb, "compile_commands.json")))}
want = [l.strip() for l in open(a.objects) if l.strip()]
ents = [db[o] for o in want if o in db]
print(f"{len(ents)}/{len(want)} objects have a compdb entry", file=sys.stderr)
os.makedirs(a.out, exist_ok=True)

def run(e):
    argv = shlex.split(e["command"])
    obj = os.path.join(a.out, e["output"].replace("/", "_"))
    out = []
    skip = False
    for i, t in enumerate(argv):
        if skip:
            skip = False; continue
        if i == 0:
            out.append(a.clang); continue
        if t == "-o":
            skip = True; continue
        if t.startswith("-o"):
            continue
        if t in ("-MD", "-MQ", "-MF") or t.startswith("-MF"):
            if t in ("-MQ", "-MF"): skip = True
            continue
        out.append(t)
    out += ["-o", obj]
    try:
        p = subprocess.run(out, cwd=e["directory"], capture_output=True, text=True, timeout=a.timeout)
        return {"obj": obj, "src": e["file"], "rc": p.returncode, "err": p.stderr[-1500:]}
    except subprocess.TimeoutExpired:
        return {"obj": obj, "src": e["file"], "rc": "timeout", "err": ""}

res = []
with cf.ThreadPoolExecutor(max_workers=a.j) as ex:
    for i, r in enumerate(ex.map(run, ents), 1):
        res.append(r)
        if i % 50 == 0: print(f"  {i}/{len(ents)}", file=sys.stderr)
json.dump(res, open(os.path.join(a.out, "_shards.json"), "w"), indent=1)
ok = [r for r in res if r["rc"] == 0 and os.path.exists(r["obj"])]
print(f"shards: {len(ok)}/{len(res)}")
c = collections.Counter()
for r in res:
    if r not in ok:
        line = next((l for l in reversed(r["err"].splitlines()) if "error" in l.lower()), r["err"][-120:])
        c[line.split(": error: ")[-1][:110]] += 1
for k, n in c.most_common(10): print(f"  {n:5d} {k}")
with open(os.path.join(a.out, "_link-line.txt"), "w") as fh:
    for r in ok: fh.write(r["obj"] + "\n")
