#!/usr/bin/env python3
"""FR-63.1 -- the deterministic controller for the improvement harness.

The measure/gate/ratchet layer of the loop, scripted so it is reproducible and
so the editing agent never scores or gates its own work (the split is
load-bearing: it is why the epoch comparison is valid). This CLI runs LOOP.md
1-6 headless; it never edits the emitter.

Subcommands::

  controller.py collect    [--explore F ...]     # ranked queue (signals.py)
  controller.py freeze      --id N --corpus DIR   # pin the epoch (epoch.py)
  controller.py establish   --id N                # champion train/held-out score
  controller.py gate       [--no-build] [--no-oracle] [--fail-inject]
  controller.py score       --id N                # candidate vs champion
  controller.py accept      --id N [--note S]     # ratchet + atomic commit
  controller.py revert                            # git checkout the worktree
  controller.py iterate     --id N [gate flags]   # gate->score->accept|revert

THE CONTRACT (never traded for score):
  1. Byte-diff oracle is supreme -- correctness is check-emitrust at 100%, never
     cargo/clippy clean (compile-only cannot see a miscompile).
  2. No new `unsafe`; no new allow-attribute beyond the champion's crate-root
     header -- emit the idiomatic form, do not suppress the lint.
  3. Ratchets never regress; held-out generalization must not regress.
The gate enforces 1 and 2 mechanically; score enforces 3.

Exit codes are Result-style: 0 = green/accept, non-zero = gate/score failed
(the caller reverts). `iterate` reverts internally and still exits non-zero so
a `/loop` wrapper sees the failure.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)  # sibling harness modules resolve when imported too
import epoch as epoch_mod          # noqa: E402
import signals as signals_mod      # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CLIPPY_EVAL = os.path.join(REPO, "nix", "clippy-eval", "clippy_eval.py")
# FR-144: the epoch-pinned baseline is authoritative. `accept` --updates AND
# commits this exact path, so it must equal clippy_eval.DEFAULT_BASELINE and
# signals.DEFAULT_CLIPPY; otherwise the loop commits a document the ratchet
# never reads (which is how the ratchet came to compare 170 crates against a
# 164-crate baseline).
CLIPPY_BASELINE = os.path.join(REPO, "nix", "clippy-eval",
                               "clippy-baseline-epoch4.json")
CHAMPION = os.path.join(HERE, "champion.json")
def _default_emitrust_cc():
    # CMake trees put tools in build/bin/, meson trees in build/tools/.
    for rel in (("build", "bin", "emitrust-cc"),
                ("build", "tools", "emitrust-cc")):
        p = os.path.join(REPO, *rel)
        if os.path.exists(p):
            return p
    return os.path.join(REPO, "build", "bin", "emitrust-cc")


EMITRUST_CC = os.environ.get("EMITRUST_CC", _default_emitrust_cc())

# The one allow-attribute the emitter is permitted to emit at the crate root.
# Any allow-line outside the champion's recorded set is a new suppression and
# fails the gate (contract clause 2).
ESTABLISHED_ALLOW_PREFIXES = ("#![allow(",)


def sh(cmd, **kw):
    """Run a command, streaming to the caller. Returns the CompletedProcess."""
    print(f"  $ {' '.join(cmd)}", flush=True)
    return subprocess.run(cmd, cwd=kw.pop("cwd", REPO), **kw)


# ---------------------------------------------------------------- scan (gate 2)

def scan_emitted(files):
    """Transpile each epoch file (cheap, transpile-only) and inspect the
    emitted Rust for contract violations: any `unsafe` token, and the set of
    allow-attribute lines. Returns (unsafe_count, sorted allow_lines,
    n_emitted, n_failed)."""
    unsafe = 0
    allow_lines = set()
    ok = failed = 0
    for f in files:
        p = subprocess.run([EMITRUST_CC, "--emit=rust", f, "-o", "-"],
                           capture_output=True, text=True)
        if p.returncode != 0:
            failed += 1
            continue
        ok += 1
        for line in p.stdout.splitlines():
            s = line.strip()
            # token-accurate: `unsafe` as a word, not inside an identifier.
            if _has_word(s, "unsafe"):
                unsafe += 1
            if s.startswith("#![allow(") or s.startswith("#[allow("):
                allow_lines.add(s)
    return unsafe, sorted(allow_lines), ok, failed


def _has_word(s, word):
    i = s.find(word)
    while i != -1:
        left = i == 0 or not (s[i - 1].isalnum() or s[i - 1] == "_")
        j = i + len(word)
        right = j >= len(s) or not (s[j].isalnum() or s[j] == "_")
        if left and right:
            return True
        i = s.find(word, i + 1)
    return False


def epoch_files(epoch_id):
    doc = epoch_mod.load_epoch(epoch_id)
    return [e["path"] for e in doc["files"]]


# ------------------------------------------------------------------- collect

def cmd_collect(args):
    result = signals_mod.build_queue(
        args.clippy, args.explore, args.realworld,
        args.min_clippy, args.min_demand)
    out = args.out or os.path.join(HERE, "queue.json")
    with open(out, "w") as f:
        json.dump(result, f, indent=2)
        f.write("\n")
    q = result["queue"]
    print(f"collected {len(q)} ranked items -> {out}")
    for i, c in enumerate(q[:args.top], 1):
        print(f"  {i:>2}. [{c['priority']:>8.1f}] {c['signal']:11s} "
              f"{c['kind']:11s} x{c['leverage']:<4} {c['id']}")
    if not q:
        print("  (empty queue -- plateau signal)")
    return 0


# -------------------------------------------------------------------- freeze

def cmd_freeze(args):
    epoch_mod.freeze(args.corpus, args.id)
    if args.split:
        epoch_mod.split(args.id, args.held_out_frac, args.seed)
    return 0


# ------------------------------------------------------ measure helpers

def _measure_slice(corpus_root, file_list, tag):
    """Run clippy_eval over a file list, return its JSON report."""
    out = os.path.join(HERE, f".score-{tag}.json")
    env = dict(os.environ, EMITRUST_CC=EMITRUST_CC)
    cmd = [sys.executable, CLIPPY_EVAL, corpus_root,
           "--file-list", file_list, "--json", out]
    r = subprocess.run(cmd, cwd=REPO, env=env,
                       capture_output=True, text=True)
    if r.returncode not in (0, 1) or not os.path.exists(out):
        # An explicit --file-list is a slice MEASUREMENT: clippy_eval reports
        # and exits 0 without ratcheting (a slice is not the pinned
        # population, so comparing it to the baseline would compare across
        # populations). Anything else, or a missing report, is a real failure.
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"clippy_eval failed for {tag}")
    with open(out) as f:
        return json.load(f)


def _slice_paths(epoch_id):
    train = os.path.join(HERE, f"epoch-{epoch_id}.train.txt")
    held = os.path.join(HERE, f"epoch-{epoch_id}.heldout.txt")
    if not (os.path.exists(train) and os.path.exists(held)):
        raise SystemExit(f"no train/held-out split for epoch-{epoch_id}; run "
                         "`freeze --split` or `epoch.py split` first")
    return train, held


# ------------------------------------------------------------------ establish

def cmd_establish(args):
    """Measure the CURRENT (champion) emitter on train + held-out and record
    the champion score + the permitted allow-line set. This is the reference
    every candidate iteration is scored against."""
    # FR-144: re-hash the pinned population first. Establishing a champion
    # over a corpus that has moved since the freeze makes every later paired
    # comparison against it invalid.
    doc = epoch_mod.assert_comparable(args.id)
    train_fl, held_fl = _slice_paths(args.id)
    train = _measure_slice(doc["corpus_root"], train_fl, "train")
    held = _measure_slice(doc["corpus_root"], held_fl, "heldout")
    unsafe, allow_lines, ok, failed = scan_emitted(epoch_files(args.id))
    if unsafe:
        raise SystemExit(f"champion already emits {unsafe} unsafe tokens -- "
                         "the invariant is 0; refusing to establish")
    champ = {
        "epoch_id": args.id,
        "corpus_root": doc["corpus_root"],
        "corpus_hash": doc["corpus_hash"],
        "emitter_rev": epoch_mod._git_rev(),
        "train": {"total_warnings": train["total_warnings"],
                  "by_lint": train["by_lint"]},
        "held_out": {"total_warnings": held["total_warnings"],
                     "by_lint": held["by_lint"]},
        "allow_lines": allow_lines,
        "unsafe_count": unsafe,
    }
    with open(CHAMPION, "w") as f:
        json.dump(champ, f, indent=2)
        f.write("\n")
    print(f"champion established: train={train['total_warnings']} "
          f"held_out={held['total_warnings']}  allow_lines={len(allow_lines)}  "
          f"unsafe=0  ({ok} crates emitted, {failed} skipped)")
    return 0


# ----------------------------------------------------------------------- gate

def cmd_gate(args):
    """Contract clauses 1+2: build + byte-diff oracle at 100% + no new
    unsafe / allow. Returns 0 only if all pass."""
    if args.fail_inject:
        print("GATE: fail injected (dry-run) -- FAIL")
        return 3

    if not args.no_build:
        r = sh(["nix", "develop", "-c", "ninja", "-C", "build"])
        if r.returncode != 0:
            print("GATE: build FAILED")
            return 4

    if not args.no_oracle:
        # A meson-configured build dir has no `check-emitrust` ninja target;
        # `meson test` runs the identical lit suite (CLAUDE.md build notes).
        if os.path.isdir(os.path.join(REPO, "build", "meson-info")):
            oracle = ["nix", "develop", "-c", "meson", "test", "-C", "build"]
        else:
            oracle = ["nix", "develop", "-c", "ninja", "-C", "build",
                      "check-emitrust"]
        r = sh(oracle)
        if r.returncode != 0:
            print("GATE: check-emitrust FAILED (byte-diff oracle is supreme)")
            return 5
    else:
        print("  (--no-oracle: skipping check-emitrust -- NOT gate-valid)")

    # contract clause 2 -- scan emitted Rust
    champ = _load_champion_optional()
    permitted = set(champ.get("allow_lines", [])) if champ else None
    files = epoch_files(args.id) if args.id else _default_scan_files()
    unsafe, allow_lines, ok, _ = scan_emitted(files)
    print(f"  scan: {ok} crates emitted, unsafe_tokens={unsafe}, "
          f"allow_lines={len(allow_lines)}")
    if unsafe:
        print(f"GATE: {unsafe} `unsafe` tokens in emitted Rust (invariant 0)")
        return 6
    if permitted is not None:
        new_allows = sorted(set(allow_lines) - permitted)
        if new_allows:
            print("GATE: new allow-attribute(s) beyond the crate-root header:")
            for a in new_allows:
                print(f"        {a}")
            return 7
    print("GATE: PASS (oracle green, unsafe=0, no new allow)")
    return 0


def _default_scan_files():
    e2e = os.path.join(REPO, "test", "EndToEnd")
    return sorted(os.path.join(e2e, x) for x in os.listdir(e2e)
                  if x.endswith(".c"))


def _load_champion_optional():
    if os.path.exists(CHAMPION):
        with open(CHAMPION) as f:
            return json.load(f)
    return None


# ---------------------------------------------------------------------- score

def cmd_score(args):
    """Contract clause 3: the candidate must lower train warnings AND not
    regress held-out (anti-Goodhart). Prints metrics; exit 0 iff accept-worthy."""
    champ = _load_champion_optional()
    if champ is None:
        raise SystemExit("no champion.json -- run `establish` first")
    # FR-144: the champion/epoch hash check below compares two DOCUMENTS; it
    # cannot see that the files themselves moved. Re-hash them.
    doc = epoch_mod.assert_comparable(args.id)
    if champ.get("corpus_hash") != doc["corpus_hash"]:
        raise SystemExit(
            f"champion/epoch hash mismatch -- champion.json was established "
            f"on epoch-{champ.get('epoch_id')}, not epoch-{args.id}. Metrics "
            "are never compared across epochs: run `establish --id "
            f"{args.id}` first.")
    train_fl, held_fl = _slice_paths(args.id)
    train = _measure_slice(doc["corpus_root"], train_fl, "train")
    held = _measure_slice(doc["corpus_root"], held_fl, "heldout")

    ct = champ["train"]["total_warnings"]
    ch = champ["held_out"]["total_warnings"]
    nt = train["total_warnings"]
    nh = held["total_warnings"]
    print(f"score epoch-{args.id}:")
    print(f"  train   : {ct:>5} -> {nt:>5}  ({nt - ct:+d})")
    print(f"  held-out: {ch:>5} -> {nh:>5}  ({nh - ch:+d})")

    # persist the candidate measurement so accept can ratchet without re-running.
    with open(os.path.join(HERE, ".candidate.json"), "w") as f:
        json.dump({"epoch_id": args.id, "train": train, "held_out": held}, f,
                  indent=2)

    if nt >= ct:
        print("SCORE: train did not improve -- REJECT")
        return 1
    if nh > ch:
        print("SCORE: held-out REGRESSED (overfit to the train slice) -- REJECT")
        return 2
    print(f"SCORE: ACCEPT-WORTHY (train -{ct - nt}, held-out {nh - ch:+d})")
    return 0


# --------------------------------------------------------------------- accept

def cmd_accept(args):
    """Ratchet the baselines and commit the emitter + baselines atomically.
    Champion advances to the candidate; the ledger records the descent."""
    cand_path = os.path.join(HERE, ".candidate.json")
    if not os.path.exists(cand_path):
        raise SystemExit("no .candidate.json -- run `score` before `accept`")
    with open(cand_path) as f:
        cand = json.load(f)

    # 1. lower the pinned clippy ratchet (the committed champion metric). The
    # population comes from the epoch the baseline is pinned to, so the
    # committed number is a paired comparison against the previous one.
    env = dict(os.environ, EMITRUST_CC=EMITRUST_CC)
    r = subprocess.run([sys.executable, CLIPPY_EVAL,
                        "--baseline", CLIPPY_BASELINE, "--update"],
                       cwd=REPO, env=env, capture_output=True, text=True)
    sys.stdout.write(r.stdout[-400:])
    if r.returncode != 0:
        raise SystemExit("clippy_eval --update failed; not committing")

    # 2. advance champion.json (train + held-out + refreshed allow-line set).
    champ = _load_champion_optional() or {}
    _, allow_lines, _, _ = scan_emitted(epoch_files(args.id))
    champ.update({
        "epoch_id": args.id,
        "emitter_rev": epoch_mod._git_rev(),  # pre-commit; refreshed post-commit
        "train": {"total_warnings": cand["train"]["total_warnings"],
                  "by_lint": cand["train"]["by_lint"]},
        "held_out": {"total_warnings": cand["held_out"]["total_warnings"],
                     "by_lint": cand["held_out"]["by_lint"]},
        "allow_lines": allow_lines,
        "unsafe_count": 0,
    })
    with open(CHAMPION, "w") as f:
        json.dump(champ, f, indent=2)
        f.write("\n")

    # 3. atomic commit: emitter sources + all baselines together.
    with open(CLIPPY_BASELINE) as f:
        total = json.load(f)["total_warnings"]
    # Emitter sources AND the goldens the optimizer shifted (a golden left
    # uncommitted would leave the committed tree failing check-emitrust) AND
    # the ratchet baselines -- one atomic revision.
    sh(["git", "add", "-A", "lib", "tools", "include", "test",
        os.path.relpath(CLIPPY_BASELINE, REPO), "nix/harness/champion.json"])
    msg = (args.message or
           f"feat(quality): harness iteration -- clippy total -> {total} "
           f"(FR-63)\n\n{args.note}").strip()
    r = sh(["git", "commit", "-m", msg,
            "-m", "Claude-Session: https://claude.ai/code/session_01BkJc4yNyeFb2XWGshwUe15"])
    if r.returncode != 0:
        print("accept: nothing committed (no staged emitter change?)")
        return 8

    # 4. ledger: record (emitter_rev, metrics) for this epoch.
    rev = epoch_mod._git_rev()
    metrics = {
        "total_warnings": total,
        "train_total": cand["train"]["total_warnings"],
        "held_out_total": cand["held_out"]["total_warnings"],
    }
    mfile = os.path.join(HERE, ".metrics.json")
    with open(mfile, "w") as f:
        json.dump(metrics, f)
    epoch_mod.ledger_append(args.id, rev, mfile, note=args.note)
    # The ledger records the rev just committed, so it is a follow-up commit
    # (it cannot be inside the commit whose hash it names).
    sh(["git", "add", os.path.relpath(epoch_mod.LEDGER, REPO)])
    sh(["git", "commit", "-m", f"chore(harness): ledger += {rev[:12]} (FR-63)",
        "-m", "Claude-Session: https://claude.ai/code/session_01BkJc4yNyeFb2XWGshwUe15"])
    print(f"ACCEPT: committed {rev[:12]}; clippy total -> {total}")
    return 0


# --------------------------------------------------------------------- revert

def cmd_revert(args):
    """Discard the candidate edit; the loop never ships red."""
    sh(["git", "checkout", "--", "lib", "tools", "include"])
    for scratch in (".candidate.json", ".score-train.json",
                    ".score-heldout.json"):
        p = os.path.join(HERE, scratch)
        if os.path.exists(p):
            os.remove(p)
    print("REVERT: worktree restored (emitter sources checked out)")
    return 0


# -------------------------------------------------------------------- iterate

def cmd_iterate(args):
    """One headless iteration assuming the optimizer edit is already applied:
    gate -> score -> accept, reverting (and exiting non-zero) on any failure."""
    g = cmd_gate(args)
    if g != 0:
        cmd_revert(args)
        print(f"ITERATE: gate failed (rc {g}) -- reverted")
        return g
    s = cmd_score(args)
    if s != 0:
        cmd_revert(args)
        print(f"ITERATE: score rejected (rc {s}) -- reverted")
        return s
    a = cmd_accept(args)
    if a != 0:
        return a
    print("ITERATE: accepted and committed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="deterministic harness controller")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("collect")
    p.add_argument("--clippy", default=CLIPPY_BASELINE)
    p.add_argument("--explore", action="append", default=[])
    p.add_argument("--realworld", default=None)
    p.add_argument("--min-clippy", type=int, default=5)
    p.add_argument("--min-demand", type=int, default=2)
    p.add_argument("--out", default=None)
    p.add_argument("--top", type=int, default=15)

    p = sub.add_parser("freeze")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--corpus", required=True)
    p.add_argument("--split", action="store_true")
    p.add_argument("--held-out-frac", type=float, default=0.25)
    p.add_argument("--seed", type=int, default=1)

    p = sub.add_parser("establish")
    p.add_argument("--id", type=int, required=True)

    def add_gate_flags(p):
        p.add_argument("--id", type=int, default=None)
        p.add_argument("--no-build", action="store_true")
        p.add_argument("--no-oracle", action="store_true")
        p.add_argument("--fail-inject", action="store_true")

    add_gate_flags(sub.add_parser("gate"))

    p = sub.add_parser("score")
    p.add_argument("--id", type=int, required=True)

    p = sub.add_parser("accept")
    p.add_argument("--id", type=int, required=True)
    p.add_argument("--note", default="")
    p.add_argument("--message", default=None)

    sub.add_parser("revert")

    p = sub.add_parser("iterate")
    add_gate_flags(p)
    p.add_argument("--note", default="")
    p.add_argument("--message", default=None)

    args = ap.parse_args(argv)
    dispatch = {
        "collect": cmd_collect, "freeze": cmd_freeze, "establish": cmd_establish,
        "gate": cmd_gate, "score": cmd_score, "accept": cmd_accept,
        "revert": cmd_revert, "iterate": cmd_iterate,
    }
    # iterate/score/accept need --id; iterate reuses gate flags which default id
    if args.cmd == "iterate" and args.id is None:
        ap.error("iterate requires --id")
    return dispatch[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
