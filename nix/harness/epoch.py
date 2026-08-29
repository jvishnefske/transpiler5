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
  epoch.py close   --id N --reason S # retire an epoch as history (FR-144)
  epoch.py status                    # which epochs are live, which are closed
  epoch.py ledger-append --id N --rev R --metrics FILE [--note S]
  epoch.py ledger-show   --id N      # render the monotone descent

THE DRIFT GUARD (FR-144). The paired comparison is only valid while the pinned
files still hold the pinned bytes. Until FR-144, ``ledger_append`` compared the
epoch DOCUMENT's stored hash against the ledger's -- never against the files on
disk -- and nothing called ``verify`` automatically, so epochs 1, 2 and 3 all
drifted unnoticed and every trajectory recorded against them was measured over
a population that had already moved. ``assert_comparable`` closes that: it
re-hashes the pinned files and REFUSES (non-zero, Result-style) if any moved,
and ``ledger_append`` -- plus the controller's establish/score and the clippy
ratchet -- call it before they record or compare. A wrong number nobody flags
is worse than no number.

CLOSED EPOCHS (FR-144). Drift is not repaired by re-freezing: rewriting a
frozen document would destroy the record the ledger's numbers were measured
against. An epoch is instead CLOSED -- retired to history in a separate
document, ``epoch-status.json``, which records when, at which rev, why, and
exactly which files had moved. A closed epoch fails ``verify`` and is refused
by every comparison, so its numbers can be read but never extended.

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
# Closure record. A FRESH document (immutable-by-default): closing an epoch
# must never edit the frozen epoch-N.json, which is the evidence the ledger's
# numbers were measured against.
STATUS = os.path.join(HERE, "epoch-status.json")


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


def drift(doc):
    """Files of a frozen epoch that no longer hold their pinned bytes.

    Returns ``[(path, "MISSING"|"CONTENT-CHANGED"), ...]``, empty when the
    population is intact. This is the one place drift is decided; verify, the
    close record and every comparison guard share it so they can never
    disagree about whether an epoch has moved.
    """
    bad = []
    for e in doc["files"]:
        if not os.path.exists(e["path"]):
            bad.append((e["path"], "MISSING"))
        elif _sha256_file(e["path"]) != e["sha256"]:
            bad.append((e["path"], "CONTENT-CHANGED"))
    return bad


def _format_drift(epoch_id, bad, limit=20):
    lines = [f"epoch-{epoch_id} DRIFTED ({len(bad)} files no longer match "
             f"the freeze):"]
    lines += [f"  {why:16s} {path}" for path, why in bad[:limit]]
    if len(bad) > limit:
        lines.append(f"  ... and {len(bad) - limit} more")
    return "\n".join(lines)


def load_status():
    """The closure record: which epochs are retired history, and why."""
    if os.path.exists(STATUS):
        with open(STATUS) as f:
            return json.load(f)
    return {"epochs": {}}


def save_status(status):
    with open(STATUS, "w") as f:
        json.dump(status, f, indent=2)
        f.write("\n")


def epoch_state(epoch_id):
    """The closure entry for an epoch, or None while it is still live."""
    return load_status()["epochs"].get(str(epoch_id))


def _last_change(path):
    """`git log -1` for a drifted file -- the WHEN of the drift, recorded once
    at closure time so the evidence survives the file's later history."""
    try:
        r = subprocess.run(["git", "log", "-1", "--date=short",
                            "--format=%h %ad %s", "--", path],
                           capture_output=True, text=True,
                           cwd=os.path.abspath(os.path.join(HERE, "..", "..")))
        return r.stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def close_epoch(epoch_id, reason, note="", timestamp=None):
    """Retire an epoch to history. Writes epoch-status.json ONLY.

    Closing is deliberately not repair: the frozen document is left byte for
    byte alone (re-freezing it would rewrite the record that the ledger's
    numbers were measured against), and any drift found at closure time is
    recorded with its blame, so the reason a trajectory is untrustworthy stays
    readable forever. A closure is itself history: re-closing is refused.
    """
    doc = load_epoch(epoch_id)
    status = load_status()
    key = str(epoch_id)
    prior = status["epochs"].get(key)
    if prior is not None:
        raise SystemExit(
            f"epoch-{epoch_id} is already closed ({prior['closed_at']}: "
            f"{prior['reason']}) -- a closure is history and is never "
            "rewritten.")
    bad = drift(doc)
    status["epochs"][key] = {
        "epoch_id": epoch_id,
        "closed_at": timestamp or datetime.datetime.now().isoformat(
            timespec="seconds"),
        "closed_at_rev": _git_rev(),
        "reason": reason,
        "note": note,
        "corpus_root": doc["corpus_root"],
        "corpus_hash": doc["corpus_hash"],
        "file_count": doc["file_count"],
        "drifted": bool(bad),
        "drifted_files": [{"path": path, "state": why,
                           "last_changed": _last_change(path)}
                          for path, why in bad],
    }
    save_status(status)
    print(f"epoch-{epoch_id} CLOSED: {reason}")
    if bad:
        print(f"  drifted at closure: {len(bad)} files")
        for d in status["epochs"][key]["drifted_files"][:20]:
            print(f"    {d['state']:16s} {d['path']}  [{d['last_changed']}]")
    print(f"  written: {STATUS}")
    return 0


def assert_comparable(epoch_id):
    """The guard every comparison must pass through.

    Raises SystemExit (Result-style non-zero) unless the epoch is still a
    valid population: live (not closed) AND its pinned files still hold their
    pinned bytes. Returns the epoch document so callers can chain.
    """
    doc = load_epoch(epoch_id)
    state = epoch_state(epoch_id)
    if state is not None:
        raise SystemExit(
            f"epoch-{epoch_id} is CLOSED ({state['closed_at']}: "
            f"{state['reason']}) -- refusing to measure or record against it. "
            "Its numbers are history, not a baseline; freeze a NEW epoch.")
    bad = drift(doc)
    if bad:
        raise SystemExit(
            _format_drift(epoch_id, bad) + "\n"
            "Refusing: a delta measured over a moved population is "
            "attributable to nothing. Freeze a NEW epoch (and `close` this "
            "one) rather than re-freezing this document.")
    return doc


def verify(epoch_id):
    doc = load_epoch(epoch_id)
    state = epoch_state(epoch_id)
    bad = drift(doc)
    recomputed = corpus_hash([{"path": e["path"], "sha256": _sha256_file(e["path"])}
                              for e in doc["files"] if os.path.exists(e["path"])])
    if state is not None:
        print(f"epoch-{epoch_id} CLOSED {state['closed_at']} "
              f"(rev {state['closed_at_rev'][:12]}): {state['reason']}")
        print("  closed epochs are history -- nothing may compare against "
              "them.")
        if bad:
            print(_format_drift(epoch_id, bad))
        return 1
    if bad:
        print(_format_drift(epoch_id, bad))
        return 1
    if recomputed != doc["corpus_hash"]:
        print(f"epoch-{epoch_id} hash mismatch: {recomputed} != {doc['corpus_hash']}")
        return 1
    print(f"epoch-{epoch_id} verified: {doc['file_count']} files reproduce "
          f"{doc['corpus_hash']}")
    return 0


def status_report():
    """Which epochs exist, which are live, which are closed history."""
    ids = sorted(int(n.split("-")[1].split(".")[0])
                 for n in os.listdir(HERE)
                 if n.startswith("epoch-") and n.endswith(".json")
                 and n != "epoch-status.json")
    status = load_status()
    for eid in ids:
        doc = load_epoch(eid)
        state = status["epochs"].get(str(eid))
        if state is None:
            bad = drift(doc)
            mark = "LIVE" if not bad else f"LIVE-BUT-DRIFTED({len(bad)})"
            extra = ""
        else:
            mark = "CLOSED"
            extra = (f"  {state['closed_at']}  {state['reason']}"
                     + (f"  [{len(state['drifted_files'])} drifted]"
                        if state["drifted"] else ""))
        print(f"epoch-{eid:<3d} {mark:22s} {doc['file_count']:>4d} files"
              f"  {doc['corpus_root']}{extra}")
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
    # FR-144: the stored-hash check below only proved the DOCUMENT had not
    # changed; it never looked at the files. Re-hash the population first, so
    # a trajectory can never be recorded over a corpus that has moved.
    doc = assert_comparable(epoch_id)
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
    # A trajectory over a population that has since moved is history, not a
    # baseline. Say so at the top of the descent rather than letting the
    # numbers be read as current evidence (FR-144).
    state = epoch_state(epoch_id)
    if state is not None:
        print(f"  CLOSED {state['closed_at']} "
              f"(rev {state['closed_at_rev'][:12]}): {state['reason']}")
        if state["drifted"]:
            print(f"  {len(state['drifted_files'])} pinned files had drifted "
                  "when it was closed -- these numbers were measured over a "
                  "MOVING population and are not comparable to any other "
                  "epoch's:")
            for d in state["drifted_files"]:
                print(f"    {d['state']:16s} {d['path']}  [{d['last_changed']}]")
    elif os.path.exists(epoch_path(epoch_id)):
        bad = drift(load_epoch(epoch_id))
        if bad:
            print(f"  WARNING: {len(bad)} pinned files have DRIFTED since the "
                  "freeze -- `close` this epoch and freeze a new one.")
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

    p = sub.add_parser("close")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--reason", required=True)
    p.add_argument("--note", default="")

    sub.add_parser("status")

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
    if args.cmd == "close":
        return close_epoch(args.id, args.reason, args.note)
    if args.cmd == "status":
        return status_report()
    if args.cmd == "ledger-append":
        return ledger_append(args.id, args.rev, args.metrics, args.note)
    if args.cmd == "ledger-show":
        return ledger_show(args.id)
    return 2


if __name__ == "__main__":
    sys.exit(main())
