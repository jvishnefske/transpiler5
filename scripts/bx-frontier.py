#!/usr/bin/env python3
"""bx-frontier: the weekly PMF indicator (FR-258) -- a per-FUNCTION frontier of
emitrust-bx over REAL C++ modules, reported with its STAGE label.

INTENT.  FR-256 made FUNCTIONS the indicator unit and named google/woff2 and
google/brunsli (Cpp2Rust's PLDI'26 evaluation programs) as the modules.  This
script pins those modules (docs/pmf/frontier/modules.toml), runs
scripts/bx-census.py's machinery on every translation unit -- the SAME
classification, function attribution, HIDDEN marking and greedy set cover, so
the two instruments never disagree about what a tag or a function is -- and
publishes one number per stage.  It obeys CLAUDE.md §3:

  * STAGES ARE NEVER MIXED.  EMIT = "a main-file function definition for which
    emitrust-bx reports no residual: none of its own, none inherited from an
    enclosing rejected class/namespace/template".  VERIFY = "additionally
    byte-identical under a per-function differential harness" -- that harness
    does not exist yet, so the VERIFY column is emitted EXPLICITLY EMPTY
    (null + status NOT_IMPLEMENTED).  It is never filled with EMIT numbers.
  * Two denominators, both labelled: EMIT/ALL functions and EMIT/OBSERVED
    functions (bodies bx actually inspected; HIDDEN ones sit inside a
    construct bx rejected whole and never looked into).
  * Every blocker set is a LOWER BOUND and every yield an UPPER BOUND (see
    bx-census's banner): rejected constructs hide their contents, and
    supporting a tag WILL expose more tags.  Set-cover rows are the ranking;
    the census's one calibration point says halve them.
  * Counting unit is the FUNCTION (deduped: one main-file definition per
    source range, so header-defined functions are not counted per includer)
    and the TU; never events.

Optional --probe-erase-namespaces: an EXPOSURE PROBE, NOT A FRONTIER.  Real
Google-style code wraps everything in `namespace X {`, which bx rejects whole,
so the true frontier hides every body.  The probe textually erases the
project's own namespaces in a COPY of each checkout (opening lines, `} //
namespace` closing lines, `X::` qualifiers, `using namespace X;`), gates every
rewritten TU on clang still accepting it, and re-runs the census, to show what
bx would see behind the namespace wall.  Its numbers are reported in a
separate, labelled section and never as the frontier.

Run inside the dev shell (emitrust-bx calls clang++ from PATH), against a
FROZEN snapshot of the tool:

    git archive HEAD tools/emitrust-bx | tar -x -C SNAP
    nix develop -c python3 scripts/bx-frontier.py \\
        --manifest docs/pmf/frontier/modules.toml --src SRC --fetch \\
        --bx SNAP/tools/emitrust-bx/emitrust-bx.py --bx-rev $(git rev-parse --short HEAD) \\
        --out OUT -j 4 [--probe-erase-namespaces]
"""

import argparse
import collections
import datetime
import fnmatch
import glob
import importlib.util
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def load_census():
    spec = importlib.util.spec_from_file_location("bx_census", os.path.join(HERE, "bx-census.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules["bx_census"] = mod  # so pickle can name census.analyze for the pool
    spec.loader.exec_module(mod)
    return mod


census = load_census()

STAGES = {
    "EMIT": ("main-file function definition for which emitrust-bx --emit=kernel reports no "
             "residual (none of its own, none inherited from an enclosing rejected "
             "class/namespace/template). Says NOTHING about rustc acceptance or behaviour."),
    "VERIFY": ("EMIT + byte-identical output against the clang-built original under a "
               "per-function differential harness. NOT IMPLEMENTED: column is empty by "
               "design and must never be filled with EMIT numbers."),
}
# bx-census's banner minus its corpus sentence (that corpus is test code; this one is real).
CAVEAT = census.LOWER_BOUND_BANNER.split("The corpus is")[0] + (
    "The corpus here is two real Google codecs (woff2, brunsli), library + CLI TUs only.\n")
VERIFY_EMPTY = {"status": "NOT_IMPLEMENTED", "functions": None, "pct": None}

# ------------------------------------------------------------------ sources


def git_head(d):
    p = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True)
    return p.stdout.strip() if p.returncode == 0 else None


def fetch(entry, dest):
    """Shallow fetch of exactly the pinned commit. Never runs project code."""
    if os.path.isdir(dest) and git_head(dest) == entry["sha"]:
        return
    if os.path.isdir(dest):
        sys.exit("refusing to touch %s: HEAD %s != pinned %s (move it away)"
                 % (dest, git_head(dest), entry["sha"]))
    os.makedirs(dest)
    for cmd in (["git", "init", "-q"], ["git", "remote", "add", "origin", entry["repo"]],
                ["git", "fetch", "-q", "--depth", "1", "origin", entry["sha"]],
                ["git", "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD"]):
        subprocess.run(cmd, cwd=dest, check=True)


def check_pins(man, src):
    for e in man.get("dep", []) + man["module"]:
        d = os.path.join(src, e["name"])
        h = git_head(d)
        if h != e["sha"]:
            sys.exit("%s: checkout HEAD %s != pinned sha %s (use --fetch)" % (d, h, e["sha"]))


def module_tus(mod, root):
    out = []
    for g in mod["tus"]:
        for p in sorted(glob.glob(os.path.join(root, g))):
            rel = os.path.relpath(p, root)
            if any(fnmatch.fnmatch(rel, x) for x in mod.get("exclude", [])):
                continue
            if rel not in out:
                out.append(rel)
    return out


def write_generated(mod, gen_root):
    d = os.path.join(gen_root, mod["name"])
    for g in mod.get("generated", []):
        p = os.path.join(d, g["path"])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            fh.write(g["content"].lstrip("\n"))
    return d if mod.get("generated") else None


def include_path(mod, man, src, mod_root, gen_dir):
    deps = {d["name"]: d for d in man.get("dep", [])}
    dirs = [os.path.join(mod_root, i) for i in mod.get("include_dirs", [])]
    if gen_dir:
        dirs.append(gen_dir)
    for dn in mod.get("deps", []):
        dirs += [os.path.join(src, dn, i) for i in deps[dn]["include_dirs"]]
    return dirs


# ------------------------------------------------------------------ probe

OPEN_RX = re.compile(r"^\s*(?:inline\s+)?namespace(?:\s+([A-Za-z_][\w:]*))?\s*\{\s*(//.*)?$")
CLOSE_RX = re.compile(r"^\s*\}\s*//\s*namespace\b.*$")
USING_RX = re.compile(r"^\s*using\s+namespace\s+([A-Za-z_][\w:]*)\s*;\s*$")


def erase_namespaces(src_root, dst_root):
    """Copy src_root to dst_root with the project's own namespaces erased.
    Line count is preserved (erased lines become empty). Returns notes."""
    if os.path.exists(dst_root):
        shutil.rmtree(dst_root)
    shutil.copytree(src_root, dst_root, ignore=shutil.ignore_patterns(".git"))
    files = []
    names = set()
    for r, _, fs in os.walk(dst_root):
        for f in fs:
            if f.endswith((".h", ".hpp", ".cc", ".cpp")):
                p = os.path.join(r, f)
                files.append(p)
                with open(p, encoding="utf-8", errors="replace") as fh:
                    for ln in fh:
                        m = OPEN_RX.match(ln)
                        if m and m.group(1):
                            names.update(m.group(1).split("::"))
    qual = re.compile(r"(?<![\w])(?:%s)::" % "|".join(sorted(map(re.escape, names)))) if names else None
    notes = []
    for p in files:
        with open(p, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
        opens = sum(1 for ln in lines if OPEN_RX.match(ln))
        closes = sum(1 for ln in lines if CLOSE_RX.match(ln))
        if opens != closes:
            notes.append("probe: %s left UNERASED (%d namespace opens vs %d `} // namespace` closes)"
                         % (os.path.relpath(p, dst_root), opens, closes))
            continue
        out = []
        for ln in lines:
            m = USING_RX.match(ln)
            if OPEN_RX.match(ln) or CLOSE_RX.match(ln) or (m and set(m.group(1).split("::")) <= names):
                out.append("")
                continue
            out.append(qual.sub("", ln) if qual else ln)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write("\n".join(out))
    notes.insert(0, "probe: erased namespaces %s" % sorted(names))
    return notes


# ------------------------------------------------------------------ sweep


def sweep(paths, args, inc_dirs):
    env_old = os.environ.get("CPLUS_INCLUDE_PATH")
    os.environ["CPLUS_INCLUDE_PATH"] = ":".join(inc_dirs)  # inherited by pool workers
    try:
        jobs = [(p, args.bx, args.clang, args.timeout) for p in paths]
        recs = []
        t0 = time.time()
        # fork, not the 3.14 default forkserver: bx-census is loaded from a hyphenated
        # path, so a fresh interpreter could not unpickle census.analyze by name.
        ctx = multiprocessing.get_context("fork")
        with ctx.Pool(args.j, maxtasksperchild=1) as pool:
            for i, rec in enumerate(pool.imap_unordered(census.analyze, jobs), 1):
                recs.append(rec)
                sys.stderr.write("[%d/%d %.0fs] %s %s\n" % (i, len(jobs), time.time() - t0,
                                                           rec["status"], rec["path"]))
    finally:
        if env_old is None:
            os.environ.pop("CPLUS_INCLUDE_PATH", None)
        else:
            os.environ["CPLUS_INCLUDE_PATH"] = env_old
    recs.sort(key=lambda r: r["path"])
    return recs


# ------------------------------------------------------------------ analysis


def pct(a, b):
    return None if not b else round(100.0 * a / b, 1)


def cover_rows(sets, steps, pof=None):
    return census.greedy_cover(sets, steps, pof)


def summarize(records, root, steps):
    """records -> stats dict. TU/function keys are relative to `root`."""
    census.REPO = root  # census.build() keys programs by relpath(path, REPO)
    B = census.build(records, None, None)
    ft, fs, pt = B["func_tags"], B["func_short"], B["prog_tags"]
    observed = {F: s for F, s in ft.items() if not fs[F][3]}
    observed_sub = {F: s for F, s in B["func_subs"].items() if not fs[F][3]}
    n_all = len(ft)
    n_obs = len(observed)
    emit_all = sum(1 for s in ft.values() if not s)
    emit_obs = sum(1 for s in observed.values() if not s)
    pof = lambda F: fs[F][0]  # noqa: E731
    st = collections.Counter(r["status"] for r in records)
    failed = [{"tu": os.path.relpath(r["path"], root), "status": r["status"],
               "detail": (r.get("detail") or "").replace(root + os.sep, "")} for r in records
              if r["status"] not in ("IN_SUBSET", "BLOCKED")]

    def tag_table(sets):
        c = collections.Counter(k for s in sets.values() for k in s)
        sole = collections.Counter(next(iter(s)) for s in sets.values() if len(s) == 1)
        return [{"tag": k, "functions": n, "sole": sole[k], "opaque": k.split(" [", 1)[0] in census.OPAQUE}
                for k, n in c.most_common()]

    return {
        "tus": {"listed": len(records), "analyzed": len(pt), "status": dict(st),
                "in_subset": sum(1 for s in pt.values() if not s), "not_analyzed": failed},
        "functions": {
            "all": n_all, "observed": n_obs, "hidden": n_all - n_obs,
            "hidden_by_tag": dict(collections.Counter(
                t for F, s in ft.items() if fs[F][3] for t in s).most_common()),
        },
        "EMIT": {
            "functions": emit_all,
            "pct_of_all_functions": pct(emit_all, n_all),
            "observed_functions": emit_obs,
            "pct_of_observed_functions": pct(emit_obs, n_obs),
            "transitively_clean_approx": len(B["trans_clean"]),
            "tus_fully_in_subset": sum(1 for s in pt.values() if not s),
        },
        "VERIFY": dict(VERIFY_EMPTY),
        "cover": {
            "functions_all": cover_rows(ft, steps, pof),
            "functions_observed": cover_rows(observed, steps, pof),
            "functions_observed_fine": cover_rows(observed_sub, steps, pof),
            "tus": cover_rows(pt, steps),
        },
        "tags_over_observed_functions": tag_table(observed),
        "fanout": {"raw_events": sum(B["tag_events"].values()),
                   "distinct_tu_tag_pairs": sum(len(s) for s in pt.values()),
                   "distinct_function_tag_pairs": sum(len(s) for s in ft.values())},
        "other_bucket": dict(B["other_events"].most_common(20)),
        "function_list": [
            {"id": F, "tu": fs[F][0], "name": fs[F][1], "hidden": fs[F][3],
             "emit": not ft[F], "verify": None, "tags": sorted(ft[F]),
             "fine_tags": sorted(B["func_subs"][F])}
            for F in sorted(ft)],
    }


def run_variant(man, args, src, label):
    """label 'frontier' -> pristine checkouts; 'probe' -> namespace-erased copies."""
    out = {"modules": {}, "notes": []}
    all_recs = []
    gen_root = os.path.join(args.out, "generated")
    for mod in man["module"]:
        orig_root = os.path.join(src, mod["name"])
        if label == "probe":
            root = os.path.join(args.out, "probe-src", mod["name"])
            out["notes"] += [mod["name"] + ": " + n for n in erase_namespaces(orig_root, root)]
        else:
            root = orig_root
        gen = write_generated(mod, gen_root)
        inc = include_path(mod, man, src, root, gen)
        tus = module_tus(mod, root)
        recs = sweep([os.path.join(root, t) for t in tus], args, inc)
        for r in recs:  # prefix with module name so the combined view has unique keys
            r["module"] = mod["name"]
        all_recs += recs
        s = summarize(recs, os.path.dirname(root), args.cover_steps)
        s["include_dirs"] = [os.path.relpath(d, args.out) if d.startswith(args.out)
                             else os.path.relpath(d, src) for d in inc]
        s["tu_list"] = tus
        out["modules"][mod["name"]] = s
        out.setdefault("_raw", {})[mod["name"]] = (recs, os.path.dirname(root))
    # combined: both modules share a parent dir in each variant
    roots = {v[1] for v in out["_raw"].values()}
    assert len(roots) == 1, roots
    out["combined"] = summarize(all_recs, roots.pop(), args.cover_steps)
    del out["combined"]["function_list"]  # the union of the per-module lists
    raw = out.pop("_raw")
    out["records"] = {m: v[0] for m, v in raw.items()} if args.keep_records else None
    return out


# ------------------------------------------------------------------ markdown


def md_cover(w, rows, n, unit):
    w("| step | +%s | from TUs | cum | blocker |" % unit)
    w("|---:|---:|---:|---:|---|")
    for i, r in enumerate(rows[:n], 1):
        star = " *OPAQUE*" if r["key"].split(" [", 1)[0] in census.OPAQUE else ""
        w("| %d | %d | %s | %d | `%s`%s |" % (i, r["marginal"], r.get("programs", "-"),
                                             r["cumulative"], r["key"], star))


def render_md(data, top):
    out = []
    w = out.append
    m = data["meta"]
    w("# emitrust-bx function frontier -- %s (bx %s)" % (m["date"], m["bx_rev"]))
    w("")
    w("Stage reported: **EMIT** only. VERIFY column is empty by design (no per-function "
      "differential harness yet). Never compare these numbers to a whole-program, "
      "runtime-checked translation as if head-to-head.")
    w("")
    w("> " + CAVEAT.replace("\n", " ").strip())
    w("")
    for lab, key in (("FRONTIER", "frontier"), ("EXPOSURE PROBE (namespaces erased -- NOT a frontier number)", "probe")):
        v = data.get(key)
        if not v:
            continue
        w("## %s" % lab)
        w("")
        w("| module | TUs listed | TUs analyzed | functions | observed | hidden | EMIT fns | "
          "EMIT % of ALL | EMIT % of OBSERVED | VERIFY |")
        w("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for name, s in list(v["modules"].items()) + [("**combined**", v["combined"])]:
            e, f, t = s["EMIT"], s["functions"], s["tus"]
            w("| %s | %d | %d | %d | %d | %d | %d | %s | %s | (empty: %s) |" % (
                name, t["listed"], t["analyzed"], f["all"], f["observed"], f["hidden"],
                e["functions"], e["pct_of_all_functions"], e["pct_of_observed_functions"],
                s["VERIFY"]["status"]))
        w("")
        for name, s in v["modules"].items():
            if s["tus"]["not_analyzed"]:
                w("TUs not analyzed in %s:" % name)
                for x in s["tus"]["not_analyzed"]:
                    w("- `%s` %s: %s" % (x["tu"], x["status"], (x["detail"] or "")[:200]))
                w("")
        for name, s in list(v["modules"].items()) + [("combined", v["combined"])]:
            w("### %s -- greedy set cover over OBSERVED functions (top %d; UPPER-bound yields, halve them)"
              % (name, top))
            w("")
            md_cover(w, s["cover"]["functions_observed"], top, "fns")
            w("")
            w("### %s -- greedy set cover over ALL functions (hidden ones carry only their container tag)" % name)
            w("")
            md_cover(w, s["cover"]["functions_all"], min(top, 5), "fns")
            w("")
            w("### %s -- greedy set cover over TUs" % name)
            w("")
            md_cover(w, s["cover"]["tus"], min(top, 5), "TUs")
            w("")
        for n in v.get("notes", []):
            w("- " + n)
        w("")
    return "\n".join(out) + "\n"


# ------------------------------------------------------------------ main


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=os.path.join(REPO, "docs", "pmf", "frontier", "modules.toml"))
    ap.add_argument("--src", required=True, help="directory holding one checkout per module/dep name")
    ap.add_argument("--fetch", action="store_true", help="shallow-fetch missing checkouts at the pinned SHAs")
    ap.add_argument("--bx", required=True, help="emitrust-bx.py of a FROZEN snapshot")
    ap.add_argument("--bx-rev", required=True, help="git short sha the snapshot was archived from")
    ap.add_argument("--clang", default="clang++")
    ap.add_argument("--out", required=True)
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--cover-steps", type=int, default=40)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--probe-erase-namespaces", action="store_true")
    ap.add_argument("--keep-records", action="store_true", help="embed raw census records in the JSON")
    ap.add_argument("--date", default=datetime.date.today().isoformat())
    args = ap.parse_args(argv)
    args.bx = os.path.abspath(args.bx)
    args.out = os.path.abspath(args.out)
    src = os.path.abspath(args.src)
    os.makedirs(args.out, exist_ok=True)
    with open(args.manifest, "rb") as fh:
        man = tomllib.load(fh)
    if args.fetch:
        for e in man.get("dep", []) + man["module"]:
            fetch(e, os.path.join(src, e["name"]))
    check_pins(man, src)
    data = {
        "meta": {
            "fr": "FR-258", "date": args.date, "bx_rev": args.bx_rev,
            "manifest": os.path.relpath(os.path.abspath(args.manifest), REPO),
            "clang": subprocess.run([args.clang, "--version"], stdout=subprocess.PIPE,
                                    text=True).stdout.splitlines()[0],
            "pins": {e["name"]: {"repo": e["repo"], "sha": e["sha"]}
                     for e in man.get("dep", []) + man["module"]},
            "stages": STAGES,
            "caveat": CAVEAT,
            "function_definition": ("main-file function definition with a body (FunctionDecl, "
                                    "CXXMethodDecl, ctor/dtor/conversion), deduped by source range "
                                    "within its TU; implicit and header-defined functions are not "
                                    "counted"),
            "hidden_definition": ("bx rejected an enclosing class/namespace/template whole, or the "
                                  "function is a top-level out-of-line method/ctor/dtor bx rejects as "
                                  "one item: bx never inspected the body, its blocker set is a pure "
                                  "lower bound"),
        },
        "frontier": run_variant(man, args, src, "frontier"),
    }
    if args.probe_erase_namespaces:
        data["probe"] = run_variant(man, args, src, "probe")
        data["probe"]["label"] = ("EXPOSURE PROBE, NOT A FRONTIER: the project's own namespaces were "
                                  "textually erased in a copy; numbers show what bx sees behind the "
                                  "namespace wall and are still LOWER-bound blocker sets")
    name = "frontier-%s-%s" % (args.date, args.bx_rev)
    with open(os.path.join(args.out, name + ".json"), "w") as fh:
        json.dump(data, fh, indent=1, sort_keys=False)
    md = render_md(data, args.top)
    with open(os.path.join(args.out, name + ".md"), "w") as fh:
        fh.write(md)
    sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
