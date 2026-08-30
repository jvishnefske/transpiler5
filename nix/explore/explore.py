#!/usr/bin/env python3
"""Diversity-aware C-input sampler for emitrust-cc -- the distinct translation
outcomes of a big corpus in the smallest time.

A real corpus is highly redundant: CMSIS-DSP's 660 files are the same handful
of shapes over many datatypes, and dozens of them raise the identical
diagnostic. Running all of them buys almost no information past the first few.
This samples the corpus to surface the DISTINCT outcomes (crashes, blocker
tags, and -- for runnable programs -- byte-diff miscompiles vs the clang-native
oracle), ranked, each with an example file and a one-line reproduction, then
stops when new outcomes plateau.

Five things make it information-dense rather than a blind pass over the tree:

  1. No repeats. emitrust-cc is deterministic, so a file runs at most once.
  2. Cheapest tier first. Each sample is a transpile-only probe
     (`--emit=rust -o -`, ~15ms) -- enough for a crash / diagnostic / clean
     signature. Only a RUNNABLE program (has main, --byte-diff on) escalates to
     the expensive build + byte-diff-vs-native tier.
  3. Stratified + novelty-guided. Files are clustered by cheap features
     (directory under the corpus root, size bucket); the sampler spends budget
     on clusters still producing NEW signatures and abandons ones that only
     reproduce known ones -- so 70 near-identical files cost a few samples, not
     seventy.
  4. Plateau early-stop. It stops when new signatures stop appearing -- and
     reports how many files it did NOT need to run.
  5. Parallel + seeded. Workers saturate the cores; a fixed --seed reproduces
     the whole sample.

Usage:
  explore.py <corpus> [--flags "ARGS"] [--seconds S | --files N] [--seed K]
             [--workers W] [--byte-diff] [--plateau S] [--recursive]

`--flags` is appended verbatim to every emitrust-cc invocation (e.g. the
include/target args a project needs: `--flags "-Iinc --extra-arg=--target=..."`).
"""
import argparse
import hashlib
import math
import os
import random
import shlex
import subprocess
import threading
import time

EMITRUST_CC = os.environ.get("EMITRUST_CC", "./build/tools/emitrust-cc")
CLANG = os.environ.get("CLANG", "clang")
CRASH_MARKERS = ("PLEASE submit a bug report", "Stack dump", "UNREACHABLE",
                 "Assertion `", "LLVM ERROR")


def blocker_tag(stderr):
    for line in stderr.splitlines():
        if "error:" in line:
            return line.split("error:", 1)[1].strip()[:70]
    return "(no diagnostic)"


class Explorer:
    def __init__(self, args):
        self.args = args
        self.rng = random.Random(args.seed)
        self.lock = threading.Lock()
        self.flags = shlex.split(args.flags)
        self.tmp = os.environ.get("TMPDIR", "/tmp")
        self.signatures = {}      # signature -> {count, file, repro, detail}
        self.new_sig_times = []
        self.seen = set()
        self.runs = 0
        self.last_new_run = 0     # self.runs at the most recent new signature
        self.start = 0.0
        self.native = {}          # file -> (has_main, stdout) cache

        files = self._enumerate(args.corpus, args.recursive)
        if not files:
            raise SystemExit("no .c/.cpp files under " + args.corpus)
        # Stratify by (top-level dir under the corpus, size bucket).
        self.clusters = {}
        for f in files:
            rel = os.path.relpath(f, args.corpus)
            top = rel.split(os.sep)[0] if os.sep in rel else "."
            try:
                bucket = int(math.log2(max(1, os.path.getsize(f))))
            except OSError:
                bucket = 0
            self.clusters.setdefault((top, bucket), []).append(f)
        for fs in self.clusters.values():
            self.rng.shuffle(fs)
        self.cstat = {k: {"i": 0, "samples": 0, "novel": 0}
                      for k in self.clusters}
        self.total_files = len(files)

    @staticmethod
    def _enumerate(corpus, recursive):
        out = []
        if recursive:
            for root, _, fs in os.walk(corpus):
                out += [os.path.join(root, f) for f in fs
                        if f.endswith((".c", ".cpp"))]
        else:
            out = [os.path.join(corpus, f) for f in os.listdir(corpus)
                   if f.endswith((".c", ".cpp"))]
        return sorted(out)

    def record(self, sig, f, repro, detail):
        with self.lock:
            if sig not in self.signatures:
                self.signatures[sig] = {"count": 0, "file": f, "repro": repro,
                                        "detail": detail}
                self.new_sig_times.append(time.time() - self.start)
                self.last_new_run = self.runs
                return True
            self.signatures[sig]["count"] += 1
            return False

    def native_stdout(self, f):
        if f in self.native:
            hm, so = self.native[f]
            return so if hm else None
        cc = "clang++" if f.endswith(".cpp") else CLANG
        std = "-std=c++17" if cc == "clang++" else "-std=c11"
        exe = os.path.join(self.tmp, "nat_" + hashlib.sha1(f.encode()).hexdigest()[:10])
        try:
            b = subprocess.run([cc, std, "-w", f, "-o", exe],
                               capture_output=True, timeout=60)
            if b.returncode != 0:
                self.native[f] = (False, None); return None
            r = subprocess.run([exe], capture_output=True, timeout=20, cwd=self.tmp)
            so = r.stdout if r.returncode == 0 else None
            self.native[f] = (True, so); return so
        except (subprocess.TimeoutExpired, OSError):
            self.native[f] = (False, None); return None

    def probe(self, f):
        """Transpile-only signature for one file."""
        cmd = [EMITRUST_CC, "--emit=rust", f, "-o", "-"] + self.flags
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except subprocess.TimeoutExpired:
            return self.record("HANG:" + os.path.basename(f), f,
                               " ".join(cmd[1:]), "transpile timed out")
        repro = "--emit=rust " + " ".join(self.flags + [os.path.relpath(f)])
        if p.returncode < 0 or any(m in p.stderr for m in CRASH_MARKERS):
            line = next((l for l in p.stderr.splitlines()
                         if any(m in l for m in CRASH_MARKERS)), p.stderr[:80])
            return self.record("CRASH:" + hashlib.sha1(line.encode()).hexdigest()[:8],
                               f, repro, line.strip()[:200])
        if p.returncode != 0:
            return self.record("REJECT:" + blocker_tag(p.stderr), f, repro,
                               blocker_tag(p.stderr))
        novel = self.record("TRANSPILED", f, repro, "")
        if self.args.byte_diff:
            novel = self.byte_diff(f) or novel
        return novel

    def byte_diff(self, f):
        native = self.native_stdout(f)
        if native is None:
            return False  # no main / native UB / non-zero exit: not an oracle
        out = os.path.join(self.tmp, "crate_" + hashlib.sha1(f.encode()).hexdigest()[:10])
        base = os.path.basename(f)
        try:
            b = subprocess.run([EMITRUST_CC, "--emit=crate", "--build", f,
                                "-o", out] + self.flags,
                               capture_output=True, text=True, timeout=180)
        except subprocess.TimeoutExpired:
            return False
        if b.returncode != 0:
            return False
        rel = os.path.join(out, "target", "release")
        exe = None
        if os.path.isdir(rel):
            for x in sorted(os.listdir(rel)):
                p = os.path.join(rel, x)
                if os.path.isfile(p) and "." not in x and os.access(p, os.X_OK):
                    exe = p; break
        if not exe:
            return False
        try:
            r = subprocess.run([exe], capture_output=True, timeout=20, cwd=self.tmp)
        except subprocess.TimeoutExpired:
            return self.record("HANG:crate:" + base, f, base, "crate hangs")
        # Oracle valid only on a clean exit; a panic (rc 101) on C UB is the
        # intended safe-failure direction, not a miscompile.
        if r.returncode == 101:
            return self.record("PANIC:" + base, f, base,
                               "crate panics (rc 101) where native exits 0")
        if r.returncode != 0:
            return self.record("CRATE-EXIT:%s:%d" % (base, r.returncode), f, base,
                               "crate exits %d, native 0" % r.returncode)
        if r.stdout != native:
            return self.record("MISCOMPILE:" + base, f, base,
                               "stdout != clang-native (both exit 0)")
        return False

    def pick(self):
        """A novelty-weighted cluster with an unsampled file left, then the next
        file in it. None when every file has been sampled."""
        with self.lock:
            live = [(k, self.cstat[k]) for k in self.clusters
                    if self.cstat[k]["i"] < len(self.clusters[k])]
            if not live:
                return None
            weights = [(1.0 + 3.0 * s["novel"]) / (1.0 + s["samples"])
                       for _, s in live]
            k = self.rng.choices([k for k, _ in live], weights=weights)[0]
            s = self.cstat[k]
            f = self.clusters[k][s["i"]]
            s["i"] += 1
            self.seen.add(f)
            return k, f

    def worker(self, deadline, max_files):
        while time.time() < deadline:
            with self.lock:
                if max_files and self.runs >= max_files:
                    return
            got = self.pick()
            if got is None:
                return
            k, f = got
            with self.lock:
                self.runs += 1
            try:
                novel = self.probe(f)
            except Exception as exc:
                novel = self.record("HARNESS-ERROR:" + type(exc).__name__, f,
                                    os.path.basename(f), str(exc)[:120])
            with self.lock:
                self.cstat[k]["samples"] += 1
                if novel:
                    self.cstat[k]["novel"] += 1
                # Plateau on EITHER a quiet wall-clock window (matters when
                # per-file cost is high, e.g. --byte-diff) OR a run of samples
                # with no new signature (matters on a big cheap redundant corpus
                # where the whole tree runs faster than the time window).
                idle_t = (time.time() - self.start) - self.new_sig_times[-1] \
                    if self.new_sig_times else 0
                idle_n = self.runs - self.last_new_run
                if (self.runs >= self.args.min_files
                        and (idle_t > self.args.plateau
                             or idle_n > self.args.plateau_files)):
                    return

    def run(self):
        self.start = time.time()
        deadline = self.start + self.args.seconds
        ts = [threading.Thread(target=self.worker, args=(deadline, self.args.files))
              for _ in range(self.args.workers)]
        for t in ts: t.start()
        for t in ts: t.join()
        self.report()

    def report(self):
        elapsed = time.time() - self.start
        sigs = self.signatures
        order = {"CRASH": 0, "HANG": 0, "MISCOMPILE": 1}
        actionable = sorted((s for s in sigs if s.split(":")[0] in order),
                            key=lambda s: order[s.split(":")[0]])
        print("=" * 70)
        print(f"sampled {self.runs} of {self.total_files} files in "
              f"{elapsed:.1f}s ({self.runs / elapsed:.0f}/s) across "
              f"{len(self.clusters)} strata; {len(sigs)} distinct signatures")
        if actionable:
            print("\n*** ACTIONABLE (crashes / hangs / miscompiles) ***")
            for s in actionable:
                print(f"  [{s}] x{sigs[s]['count'] + 1}  e.g. "
                      f"{os.path.relpath(sigs[s]['file'], self.args.corpus)}")
                print(f"    {sigs[s]['detail']}")
                print(f"    repro: {EMITRUST_CC} {sigs[s]['repro']}")
        runtime = sorted(s for s in sigs if s.split(":")[0] in ("PANIC", "CRATE-EXIT"))
        for s in runtime:
            print(f"  [{s}] (informational -- crate defended vs C UB)")
        rej = sorted((s for s in sigs if s.startswith("REJECT")),
                     key=lambda s: -sigs[s]["count"])
        tr = sigs.get("TRANSPILED", {}).get("count", 0)
        print(f"\ntranspiled clean: {tr + (1 if 'TRANSPILED' in sigs else 0)} "
              f"samples;  distinct diagnostics (coverage): {len(rej)}")
        for s in rej:
            print(f"  x{sigs[s]['count'] + 1:<4} e.g. "
                  f"{os.path.basename(sigs[s]['file']):<28} {s[7:]}")
        remaining = self.total_files - self.runs
        idle_t = elapsed - self.new_sig_times[-1] if self.new_sig_times else 0
        idle_n = self.runs - self.last_new_run
        if idle_t > self.args.plateau or idle_n > self.args.plateau_files:
            print(f"\nPLATEAUED after {idle_n} samples / {idle_t:.0f}s with no "
                  f"new outcome. Found all {len(sigs)} distinct outcomes from "
                  f"{self.runs} samples; stopped {remaining} files early.")
        else:
            print(f"\nbudget exhausted with {remaining} files unsampled "
                  f"(new signatures still arriving -- raise --seconds/--files).")
        print("new-signature times (s): "
              f"{[round(t, 1) for t in self.new_sig_times]}")
        if self.args.json:
            self.dump_json(self.args.json)

    def dump_json(self, path):
        """Machine-readable signatures for the FR-63 signal merger
        (nix/harness/signals.py). ``count`` is occurrences-1 in the same
        convention the text report uses (+1 for the exemplar); the merger
        adds one back. Paths are corpus-relative for stability."""
        import json
        sigs = {s: {"count": d["count"],
                    "file": os.path.relpath(d["file"], self.args.corpus),
                    "repro": d["repro"], "detail": d["detail"]}
                for s, d in self.signatures.items()}
        with open(path, "w") as f:
            json.dump({"corpus": self.args.corpus, "runs": self.runs,
                       "total_files": self.total_files, "signatures": sigs},
                      f, indent=2)
            f.write("\n")
        print(f"signatures written: {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--flags", default="")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--files", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--byte-diff", action="store_true")
    ap.add_argument("--recursive", action="store_true")
    ap.add_argument("--json", default=None,
                    help="dump distinct signatures as JSON (for the FR-63 "
                         "signal merger, nix/harness/signals.py)")
    ap.add_argument("--min-files", type=int, default=30)
    ap.add_argument("--plateau", type=float, default=15.0,
                    help="stop after this many idle SECONDS with no new outcome")
    ap.add_argument("--plateau-files", type=int, default=120,
                    help="stop after this many idle SAMPLES with no new outcome")
    Explorer(ap.parse_args()).run()


if __name__ == "__main__":
    main()
