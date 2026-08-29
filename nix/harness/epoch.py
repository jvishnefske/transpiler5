#!/usr/bin/env python3
"""FR-63.2 -- epoch freeze + trajectory ledger for the improvement harness.

An *epoch* is the discovery corpus pinned once: the exact file list plus a
content hash. Because the corpus is frozen, the warning delta between two
emitter revisions is attributable to the emitter change alone -- a paired
comparison. Re-sampling the corpus starts a NEW epoch; metrics are NEVER
compared across epochs (the population changed), which is why the ledger keys
every trajectory by ``epoch_id``.

This module is the deterministic memory of the loop. It has no LLM in it and
no knowledge of the emitter -- it only pins corpora, splits them
reproducibly, and records ``(emitter_rev, metrics)`` per accepted revision so
the descent is queryable (`ledger-show`).

Subcommands::

  epoch.py freeze  --corpus DIR [--corpus DIR ...] [--ext .c --ext .cpp]
                   [--recursive] [--exclude FILE] --id N [--out FILE]
  epoch.py verify  --id N            # recompute hashes, confirm the sample
  epoch.py split   --id N --held-out-frac F --seed K   # train/held-out lists
  epoch.py ledger-append --id N --rev R --metrics FILE [--note S]
  epoch.py ledger-show   --id N      # render the monotone descent

UNION CORPORA (epoch-4 and later). ``--corpus`` and ``--ext`` both repeat, so
one epoch can pin the union of several roots and several source extensions
(e.g. ``test/EndToEnd`` x {.c, .cpp}). The union is deduplicated and sorted,
so enumeration order never depends on ``os.listdir`` order, and the corpus
hash still covers every pinned file's content -- editing any pinned file
breaks ``verify``. Epochs 1-3 were frozen single-root/``.c``; the defaults
reproduce exactly that behaviour, and those documents are frozen history that
must never be rewritten.

MEASURABILITY (what ``--exclude`` is for). The epoch's metric is a clippy
tally over the crate each pinned file emits, so a file is *measurable* only if
(a) ``emitrust-cc --emit=crate`` actually produces a crate, and (b) ``cargo
clippy`` on that crate yields a COMPLETE tally -- it may fail on a
deny-by-default clippy lint (that failure IS the tally) but must not fail on a
rustc error, which would truncate the count and leave the metric undefined.
An unmeasurable file must not be pinned: ``clippy_eval`` silently skips it, so
pinning it would put a file in the denominator that contributes nothing.
Measurability is a property of the pinning revision, and the exclusion list is
recorded in the epoch document as provenance -- a file that starts
transpiling later does NOT join this epoch (that would change the population,
i.e. start a new epoch).

Design constraints (CLAUDE.md): immutable-by-default (every write is a fresh
JSON document), Result-style exit codes (non-zero on any inconsistency), no
heap-of-object-graphs -- flat dicts keyed by stable ids.
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = os.path.join(HERE, "ledger.json")


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _enumerate(roots, exts=(".c",), recursive=False, exclude=()):
    """Deterministic union enumeration over one or more roots/extensions.

    Returns a sorted, deduplicated path list. Sorted (not listdir order) so a
    re-freeze of an unchanged tree reproduces the identical file list and
    hence the identical corpus hash; deduplicated so overlapping roots cannot
    double-count a file into the denominator.
    """
    exts = tuple(exts)
    skip = {os.path.normpath(p) for p in exclude}
    found = set()
    for root in roots:
        if recursive:
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames.sort()
                for name in filenames:
                    if name.endswith(exts):
                        found.add(os.path.normpath(
                            os.path.join(dirpath, name)))
        else:
            for name in os.listdir(root):
                path = os.path.join(root, name)
                if name.endswith(exts) and os.path.isfile(path):
                    found.add(os.path.normpath(path))
    return sorted(found - skip)


def _enumerate_c(corpus):
    """Epoch-1..3 behaviour: one directory, ``.c`` only, non-recursive."""
    return _enumerate([corpus])


def read_path_list(path):
    """Newline-separated paths; ``#`` comments and blanks ignored.

    Same convention as ``clippy_eval.py --file-list`` and the split slices, so
    a held-out list can be fed straight back in as an exclusion list.
    """
    with open(path) as f:
        return [ln.strip() for ln in f
                if ln.strip() and not ln.lstrip().startswith("#")]


def _git_rev():
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"],
                              capture_output=True, text=True,
                              cwd=HERE).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def epoch_path(epoch_id):
    return os.path.join(HERE, f"epoch-{epoch_id}.json")


def corpus_hash(entries):
    """Aggregate hash pinning both the file set and each file's content.

    Order-independent: the per-file lines are sorted before hashing so a
    re-freeze of the identical sample reproduces the identical hash.
    """
    lines = sorted(f"{e['path']} {e['sha256']}" for e in entries)
    return "sha256:" + hashlib.sha256("\n".join(lines).encode()).hexdigest()


def freeze(corpus, epoch_id, out=None, exts=None, recursive=False,
           exclude=()):
    """Pin a corpus. ``corpus`` is a root dir or a list of root dirs."""
    roots = [corpus] if isinstance(corpus, str) else list(corpus)
    # Canonicalize so the epoch DOCUMENT is a pure function of the sample too,
    # not just its hash: --ext .cpp --ext .c and --ext .c --ext .cpp record
    # the same thing.
    exts = tuple(sorted(set(exts or (".c",))))
    exclude = sorted({os.path.normpath(p) for p in exclude})
    files = _enumerate(roots, exts, recursive, exclude)
    if not files:
        raise SystemExit(
            f"no {'/'.join(exts)} files under {', '.join(roots)}")
    entries = [{"path": f, "sha256": _sha256_file(f)} for f in files]
    doc = {
        "epoch_id": epoch_id,
        # corpus_root stays a plain string for a single root (epoch-1..3
        # shape, and what the ledger keys on); corpus_roots is the union.
        "corpus_root": roots[0] if len(roots) == 1 else "+".join(roots),
        "corpus_roots": roots,
        "extensions": list(exts),
        "recursive": recursive,
        "created_rev": _git_rev(),
        "corpus_hash": corpus_hash(entries),
        "file_count": len(entries),
        "excluded_count": len(exclude),
        "excluded": exclude,
        "files": entries,
    }
    out = out or epoch_path(epoch_id)
    with open(out, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"epoch-{epoch_id} frozen: {len(entries)} files"
          f"{f' (+{len(exclude)} excluded)' if exclude else ''}, "
          f"{doc['corpus_hash']}")
    print(f"  written: {out}")
    return doc


def load_epoch(epoch_id):
    p = epoch_path(epoch_id)
    if not os.path.exists(p):
        raise SystemExit(f"no such epoch: {p} (freeze it first)")
    with open(p) as f:
        return json.load(f)


def verify(epoch_id):
    doc = load_epoch(epoch_id)
    bad = []
    for e in doc["files"]:
        if not os.path.exists(e["path"]):
            bad.append((e["path"], "MISSING"))
        elif _sha256_file(e["path"]) != e["sha256"]:
            bad.append((e["path"], "CONTENT-CHANGED"))
    recomputed = corpus_hash([{"path": e["path"], "sha256": _sha256_file(e["path"])}
                              for e in doc["files"] if os.path.exists(e["path"])])
    if bad:
        print(f"epoch-{epoch_id} DRIFTED ({len(bad)} files):")
        for path, why in bad[:20]:
            print(f"  {why:16s} {path}")
        return 1
    if recomputed != doc["corpus_hash"]:
        print(f"epoch-{epoch_id} hash mismatch: {recomputed} != {doc['corpus_hash']}")
        return 1
    print(f"epoch-{epoch_id} verified: {doc['file_count']} files reproduce "
          f"{doc['corpus_hash']}")
    return 0


def split(epoch_id, held_out_frac, seed):
    """Deterministic train/held-out partition of the epoch's files.

    The partition is a pure function of (seed, path) -- no RNG state, no file
    order dependence -- so the same seed always yields the same held-out set,
    and adding a file never reshuffles the rest. The optimizer subagent is
    shown ONLY the train list; the controller scores both and rejects any
    change that regresses held-out (the anti-Goodhart guard, FR-63.3).
    """
    doc = load_epoch(epoch_id)
    paths = [e["path"] for e in doc["files"]]

    def rank(path):
        key = f"{seed}:{path}".encode()
        return int.from_bytes(hashlib.sha256(key).digest()[:8], "big")

    ordered = sorted(paths, key=rank)
    n_held = max(1, round(len(ordered) * held_out_frac))
    held = sorted(ordered[:n_held])
    train = sorted(ordered[n_held:])

    train_path = os.path.join(HERE, f"epoch-{epoch_id}.train.txt")
    held_path = os.path.join(HERE, f"epoch-{epoch_id}.heldout.txt")
    header = (f"# epoch-{epoch_id} {{}} slice -- seed={seed} "
              f"held_out_frac={held_out_frac}\n# {doc['corpus_hash']}\n")
    with open(train_path, "w") as f:
        f.write(header.format("TRAIN"))
        f.write("\n".join(train) + "\n")
    with open(held_path, "w") as f:
        f.write(header.format("HELD-OUT"))
        f.write("\n".join(held) + "\n")
    print(f"split epoch-{epoch_id}: {len(train)} train, {len(held)} held-out "
          f"(seed={seed})")
    print(f"  {train_path}")
    print(f"  {held_path}")
    return train_path, held_path


def load_ledger():
    if os.path.exists(LEDGER):
        with open(LEDGER) as f:
            return json.load(f)
    return {"epochs": {}}


def save_ledger(ledger):
    with open(LEDGER, "w") as f:
        json.dump(ledger, f, indent=2)
        f.write("\n")


def ledger_append(epoch_id, rev, metrics_path, note="", timestamp=None):
    doc = load_epoch(epoch_id)
    with open(metrics_path) as f:
        metrics = json.load(f)
    ledger = load_ledger()
    key = str(epoch_id)
    ep = ledger["epochs"].setdefault(key, {
        "corpus_root": doc["corpus_root"],
        "corpus_hash": doc["corpus_hash"],
        "revisions": [],
    })
    if ep["corpus_hash"] != doc["corpus_hash"]:
        raise SystemExit(
            f"epoch-{epoch_id} hash changed since the ledger was opened -- "
            "that is a NEW epoch; do not append across epochs.")
    ts = timestamp or datetime.datetime.now().isoformat(timespec="seconds")
    ep["revisions"].append({
        "emitter_rev": rev,
        "timestamp": ts,
        "metrics": metrics,
        "note": note,
    })
    save_ledger(ledger)
    total = metrics.get("total_warnings", "?")
    print(f"ledger: epoch-{epoch_id} += rev {rev[:12]} "
          f"(total_warnings={total})  n={len(ep['revisions'])}")
    return 0


def ledger_show(epoch_id):
    ledger = load_ledger()
    ep = ledger["epochs"].get(str(epoch_id))
    if not ep:
        print(f"epoch-{epoch_id}: no recorded revisions")
        return 1
    print(f"epoch-{epoch_id}  corpus={ep['corpus_root']}  {ep['corpus_hash']}")
    print(f"{'rev':14s} {'total':>7s} {'delta':>7s}  {'held_out':>8s}  when")
    prev = None
    for r in ep["revisions"]:
        m = r["metrics"]
        total = m.get("total_warnings")
        held = m.get("held_out_total", "-")
        delta = "" if prev is None else f"{total - prev:+d}"
        print(f"{r['emitter_rev'][:14]:14s} {total:>7} {delta:>7}  "
              f"{str(held):>8}  {r['timestamp']}  {r['note']}")
        prev = total
    if len(ep["revisions"]) >= 2:
        first = ep["revisions"][0]["metrics"]["total_warnings"]
        last = ep["revisions"][-1]["metrics"]["total_warnings"]
        mono = all(a["metrics"]["total_warnings"] >= b["metrics"]["total_warnings"]
                   for a, b in zip(ep["revisions"], ep["revisions"][1:]))
        print(f"\ndescent: {first} -> {last} ({last - first:+d})  "
              f"monotone={'yes' if mono else 'NO -- a revision regressed'}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="epoch freeze + trajectory ledger")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("freeze")
    p.add_argument("--corpus", required=True, action="append",
                   help="corpus root; repeat for a union corpus")
    p.add_argument("--ext", action="append", default=None,
                   help="source extension to pin (default .c); repeatable")
    p.add_argument("--recursive", action="store_true",
                   help="walk subdirectories of each root")
    p.add_argument("--exclude", default=None,
                   help="file of paths to omit (unmeasurable files); "
                        "'#' comments ignored")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--out", default=None)

    p = sub.add_parser("verify")
    p.add_argument("--id", type=int, required=True)

    p = sub.add_parser("split")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--held-out-frac", type=float, default=0.25)
    p.add_argument("--seed", type=int, default=1)

    p = sub.add_parser("ledger-append")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--rev", required=True)
    p.add_argument("--metrics", required=True)
    p.add_argument("--note", default="")

    p = sub.add_parser("ledger-show")
    p.add_argument("--id", type=int, required=True)

    args = ap.parse_args(argv)
    if args.cmd == "freeze":
        excl = read_path_list(args.exclude) if args.exclude else ()
        freeze(args.corpus, args.id, args.out, exts=args.ext,
               recursive=args.recursive, exclude=excl)
        return 0
    if args.cmd == "verify":
        return verify(args.id)
    if args.cmd == "split":
        split(args.id, args.held_out_frac, args.seed)
        return 0
    if args.cmd == "ledger-append":
        return ledger_append(args.id, args.rev, args.metrics, args.note)
    if args.cmd == "ledger-show":
        return ledger_show(args.id)
    return 2


if __name__ == "__main__":
    sys.exit(main())
