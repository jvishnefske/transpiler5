#!/usr/bin/env python3
"""Measurement driver for EmitRust paper data (E2/E3/E6)."""
import csv, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from runline import endtoend_projects

ROOT = Path("/home/j/src/transpiler5/.claude/worktrees/agent-a4ffe4dae7c4ea8b1")
CC = ROOT / "build/bin/emitrust-cc"
SCRATCH = Path("/tmp/claude-1000/-home-j-src-transpiler5/d2356c09-4002-4cb9-a521-40bb1f03efef/scratchpad")
WORK = SCRATCH / "v3work"
OUT = SCRATCH / "paper-data-v3"
TIMEOUT = 300


ENDTOEND_SKIPPED = []


def projects():
    """Yield dicts: corpus, name, cwd, inputs(list of str args), tus."""
    ps = []
    # cpp real-world (compdb, relative paths -> cwd must be project dir)
    for d in sorted((ROOT / "test/RealWorld/Cpp/Inputs").iterdir()):
        if not d.is_dir():
            continue
        db = json.loads((d / "compile_commands.json").read_text())
        ps.append(dict(corpus="cpp-realworld", name=d.name, cwd=str(d),
                       inputs=["--compdb", "."], tus=len(db)))
    # c real-world
    base = ROOT / "test/RealWorld/Inputs"
    for e in sorted(base.iterdir()):
        if e.is_dir():
            srcs = sorted(str(p) for p in e.glob("*.c"))
            ps.append(dict(corpus="c-realworld", name=e.name, cwd=str(base),
                           inputs=srcs, tus=len(srcs)))
        elif e.suffix == ".c":
            ps.append(dict(corpus="c-realworld", name=e.stem, cwd=str(base),
                           inputs=[str(e)], tus=1))
    # EndToEnd: input set derived from each test's own lit RUN line, so the
    # deliberately multi-TU cases get both translation units (v2 fed them only
    # %s and so manufactured undefined symbols).  See ../runline.py.
    e2e_ps, e2e_skipped = endtoend_projects(ROOT)
    ps += e2e_ps
    ENDTOEND_SKIPPED.clear()
    ENDTOEND_SKIPPED.extend(e2e_skipped)
    # c-testsuite
    cts = ROOT / "third_party/c-testsuite/tests/single-exec"
    for p in sorted(cts.glob("*.c")):
        ps.append(dict(corpus="c-testsuite", name=p.name, cwd=str(cts),
                       inputs=[str(p)], tus=1))
    # synthetic project tests (for E3 positive cases)
    proj = ROOT / "test/Project"
    for n in ["search-backtrack.cpp", "search-false-red.cpp", "search-red.c",
              "search-green.c", "search-false-green.c", "search-bound.c",
              "search-determinism.c", "search-cli.c"]:
        p = proj / n
        if p.exists():
            ps.append(dict(corpus="synthetic", name=n, cwd=str(proj),
                           inputs=[str(p)], tus=1))
    return ps


def run(args, cwd, timeout=TIMEOUT):
    t0 = time.perf_counter()
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout)
        ms = (time.perf_counter() - t0) * 1000.0
        return r.returncode, r.stdout, r.stderr, ms
    except subprocess.TimeoutExpired:
        return 124, "", "TIMEOUT", timeout * 1000.0


def crate_ok(d: Path):
    return (d / "Cargo.toml").exists() and (d / "src").is_dir()


def crate_kind(d: Path):
    """FR-51: 'lib' when the crate has src/lib.rs, 'bin' when src/main.rs.

    Recorded in the raw JSON only.  It is deliberately NOT a column of
    e2_yield.csv (the schema is frozen for comparability with the prior run),
    but it is what separates a library crate that merely compiles from a
    binary crate that can be run and differentially tested.
    """
    if (d / "src" / "lib.rs").exists():
        return "lib"
    if (d / "src" / "main.rs").exists():
        return "bin"
    return ""


def cargo_build(d: Path):
    return run(["cargo", "build", "--release", "--offline"], cwd=str(d))


def measure(p, timed_reps=1):
    """Run everything for one project.  timed_reps=3 -> best-of-3 wall clock."""
    key = f"{p['corpus']}__{p['name']}".replace("/", "_")
    wd = WORK / key
    if wd.exists():
        shutil.rmtree(wd)
    wd.mkdir(parents=True)
    res = dict(p)
    res.pop("inputs"); res.pop("cwd")

    def cc(extra, outdir=None):
        a = [str(CC)] + extra + p["inputs"]
        if outdir:
            a += ["-o", str(outdir)]
        return a

    # ---- E2: strict ----
    sdir = wd / "strict"
    rc, so, se, _ = run(cc(["--emit=crate"], sdir), p["cwd"])
    res["strict_ok"] = int(rc == 0 and crate_ok(sdir))
    res["strict_kind"] = crate_kind(sdir) if res["strict_ok"] else ""
    res["strict_builds"] = 0
    res["strict_err"] = (se.strip().splitlines() or [""])[-1][:200] if rc else ""
    if res["strict_ok"]:
        crc, _, cse, _ = cargo_build(sdir)
        res["strict_builds"] = int(crc == 0)
        if crc:
            res["strict_build_err"] = cse.strip().splitlines()[-1][:200] if cse.strip() else ""

    # ---- E2/E6: incremental (timed) ----
    idir = wd / "incr"
    best = None
    for i in range(timed_reps):
        if idir.exists():
            shutil.rmtree(idir)
        rc, so, se, ms = run(cc(["--emit=crate", "--incremental"], idir), p["cwd"])
        best = ms if best is None else min(best, ms)
    res["incremental_ms"] = round(best, 1)
    res["incr_emitted"] = int(rc == 0 and crate_ok(idir))
    res["incr_kind"] = crate_kind(idir) if res["incr_emitted"] else ""
    res["incr_rc"] = rc
    for k in ("graph_items", "ported", "stubbed", "dropped", "missing",
              "declared", "ported_permille"):
        res[k] = ""
    pj = idir / "emitrust-progress.json"
    if pj.exists():
        try:
            t = json.loads(pj.read_text())["totals"]
            for k in ("graph_items", "ported", "stubbed", "dropped", "missing",
                      "declared", "ported_permille"):
                res[k] = t.get(k, "")
        except Exception:
            pass
    res["incr_builds"] = 0
    res["cargo_ms"] = ""
    if res["incr_emitted"]:
        crc, _, cse, cms = cargo_build(idir)
        res["incr_builds"] = int(crc == 0)
        res["cargo_rc"] = crc
        res["cargo_ms"] = round(cms, 1)
        if crc:
            res["incr_build_err"] = "\n".join(
                l for l in cse.splitlines() if l.startswith("error"))[:400]

    # ---- E3: search on ----
    xdir = wd / "search"
    best = None
    for i in range(timed_reps):
        if xdir.exists():
            shutil.rmtree(xdir)
        rc, so, se, ms = run(cc(["--emit=crate", "--incremental", "--search",
                                 "--search-trace=-"], xdir), p["cwd"])
        best = ms if best is None else min(best, ms)
    res["search_ms"] = round(best, 1)
    res["search_emitted"] = int(rc == 0 and crate_ok(xdir))
    res["search_kind"] = crate_kind(xdir) if res["search_emitted"] else ""
    m = re.search(r"^summary probes=(\d+) generated=(\d+) pruned=(\d+)",
                  se, re.M)
    res["probes"] = int(m.group(1)) if m else ""
    res["generated"] = int(m.group(2)) if m else ""
    m2 = re.search(r"^best \d+ ported=(\d+) stubbed=(\d+)", se, re.M)
    res["search_ported"] = int(m2.group(1)) if m2 else ""
    res["search_improved"] = 1 if re.search(r"improved=yes", se) else 0
    sp = xdir / "emitrust-progress.json"
    res["search_graph_items"] = ""
    res["search_ported_total"] = ""
    if sp.exists():
        try:
            t = json.loads(sp.read_text())["totals"]
            res["search_graph_items"] = t.get("graph_items", "")
            res["search_ported_total"] = t.get("ported", "")
        except Exception:
            pass
    res["search_builds"] = 0
    if res["search_emitted"]:
        crc, _, _, _ = cargo_build(xdir)
        res["search_builds"] = int(crc == 0)

    # ---- E6: item-graph / coloring (timed) ----
    best = None
    for i in range(timed_reps):
        rc, so, se, ms = run(cc(["--emit=item-graph", "-o", "-"]), p["cwd"])
        best = ms if best is None else min(best, ms)
    res["graph_ms"] = round(best, 1)
    res["graph_rc"] = rc
    res["nodes"] = sum(1 for l in so.splitlines() if l.startswith("node "))
    res["edges"] = sum(1 for l in so.splitlines() if l.startswith("edge "))
    best = None
    for i in range(timed_reps):
        rc, so, se, ms = run(cc(["--emit=coloring", "-o", "-"]), p["cwd"])
        best = ms if best is None else min(best, ms)
    res["coloring_ms"] = round(best, 1)
    res["coloring_rc"] = rc

    shutil.rmtree(wd, ignore_errors=True)
    return res


def main():
    mode = sys.argv[1]
    WORK.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    ps = projects()
    # provenance for the discovery fix: what the RUN-line parser resolved, and
    # what it refused to guess at.
    with (OUT / "e2e_discovery.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["project", "tus", "inputs"])
        for p in ps:
            if p["corpus"] == "endtoend":
                w.writerow([p["name"], p["tus"],
                            " ".join(Path(i).name for i in p["inputs"])])
    with (OUT / "e2e_discovery_skipped.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["project", "reason"])
        w.writerows(ENDTOEND_SKIPPED)
    if mode == "list":
        for p in ps:
            print(p["corpus"], p["name"], p["tus"])
        return
    only = sys.argv[2] if len(sys.argv) > 2 else None
    reps = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    if only and only != "all":
        ps = [p for p in ps if p["corpus"] == only]
    out = []
    for i, p in enumerate(ps):
        r = measure(p, timed_reps=reps)
        out.append(r)
        print(f"[{i+1}/{len(ps)}] {p['corpus']}/{p['name']} "
              f"strict={r['strict_ok']}/{r['strict_builds']} "
              f"incr={r['incr_emitted']}/{r['incr_builds']} "
              f"items={r['graph_items']} ported={r['ported']} "
              f"probes={r['probes']}", flush=True)
    dest = SCRATCH / f"v3_raw_{mode}_{only or 'all'}.json"
    dest.write_text(json.dumps(out, indent=1))
    print("wrote", dest)


if __name__ == "__main__":
    main()
