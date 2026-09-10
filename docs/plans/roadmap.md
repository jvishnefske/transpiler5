# TRACTOR roadmap

State at `411d987`, 2026-09-10. This is the human-readable companion to the
two authoritative files: `design.md` is the prose evidence ledger, and
`docs/plans/backlog.toml` is the machine index. **Where they disagree with
this document, they win** — this one summarises, they record.

## Where the score is

| | |
|---|---|
| **TRACTOR PASS** | **60 / 252** (exec 24, lib 36) |
| EMIT-cleared | 74 / 252 |
| SYMBOL_MISSING | 13 — emit clean, export refused |
| VACUOUS_PASS | 1 (`update_md5_lib`; its only vector is `has_ub` and skipped) |

One session moved this **41 → 60**: FR-224's libc shim table (+4), FR-229's
char-pointer byte view in three waves (+5, +5, +2), FR-230's `alloca` (+2) and
`strchr` cursor bind (+1). FR-228 and FR-232 landed at +0 PASS by design.

### The denominator is not what it looks like

**252 cases are 125 distinct programs.** `test_case` is a symlink in 127 of
them, and one program — SPHINCS+ — is 128 cases, 51% of the corpus. So the
score reads honestly as **57/124 ordinary programs plus 0/128 SPHINCS+**, and
any per-case ranking is half-dominated by one program's configuration count
(FR-225).

## What the instrument can and cannot tell you

`scripts/tractor-census.py` gives per-case blocker sets and a set-cover
ranking. Two properties matter more than any number it prints:

- **Every set is a LOWER bound and every yield an UPPER bound.** The importer
  bails at the first error inside any item it rejects, and every non-passing
  case has at least one rejected item. There is no "confirmed" subset; a
  column claiming otherwise was ~92% wrong and has been removed (FR-223).
- **It has one calibration point: it predicted +14 EMIT for the system-header
  fix and FR-224 delivered +7.** Halve its figures until there are more.

Its `REJECTED ITEMS PER CASE` histogram is the cheap progress signal, because
it moves after each individual fix rather than only when a whole set lands:

```
60 cases  0 items   ← never emitted a crate (the openssl parse failures)
35        1         ← shallow; a bound worth trusting
15        2
 1        3
 4        4
16        7    16   8    16  14    32  15   ← the 80 SPHINCS+ lib cases
```

## Ranked work

### 1. ~~FR-230 item 2 — `strchr` cursor bind~~ · **DONE, +1 PASS**

Landed. "Price E and F as real frontiers" was pessimistic: **F never fires for
the hosted call at all**, and E was one arm of ~14 lines.

### 2. ~~FR-229 residue — `scanf %f`~~ · **DONE, +2 PASS**

Family complete at 12/12. FR-183's refusal was right and its reason survives —
resolved with a greedy C-syntax scan plus Rust's correctly-rounded parser and a
**loud panic** on hex floats and `nan(payload)`, the two forms Rust cannot
reproduce. 120,000 random values byte-identical; 119 panics, zero silent
divergences. Hex floats are deliberately *not* implemented: glibc accepts
`0x.p3` as 0, outside the C grammar, so a scanner would still diverge.

### 3. FR-226 — the SPHINCS+ package · up to +12 PASS, cost unknown

Five importer fixes plus the (already landed) unaccessed-pointer export class.
**Do not commit to the set up front.** Each of the 12 blake cases hides
fourteen stubbed functions, so "+12" is a loose upper bound. Fix **one** class,
re-measure the stub count, and re-cost — the histogram makes that cheap now.

### 4. ~~FR-228 — the stdout buffering model~~ · **DONE, EMIT +1 / PASS +0**

Landed 2026-09-10. Two things worth carrying forward. **A `BufWriter` does not
reproduce C** — glibc fills its buffer and flushes only when a write no longer
*fits*, then passes whole blocks through; measured at six boundaries, a
`BufWriter` matches one of them. And **`std::process::exit` runs no
destructors**, so every exit path needed an explicit flush spliced in
*unconditionally* — a shard cannot know whether it will be linked into a bin,
and guessing wrong truncates silently. 482 pre-existing goldens moved, all
verified to carry the runtime and re-checked by their own oracles.

### Not to be funded for yield

- **FR-230 item 3 — the pointer-member load.** +4 EMIT, **+0 PASS, and the +0
  is structural**: the only admitted pointer-struct-member model represents
  the field as an index into an owner array proven inside the unit, and every
  affected case is a `lib` entry point whose caller supplies a real address.
- **FR-227 — the multi-reference export wall.** 67 cases, 27% of the corpus,
  behind FR-181's *measured miscompile* (native 104 against export 10, exit 0,
  no diagnostic). FR-222's mutability insight rescues exactly one of them,
  because one `&mut` aliasing one `&` is UB precisely as much as two `&mut`.
  `restrict` is the real language-level escape and **none of the 67 declare
  it**.
- **openssl in the devshell.** Yields zero cases by itself; it converts 48
  unmeasurable cases into measurable ones, which then face the whole SPHINCS+
  package over a 28-file closure plus a working `main`.

## Open, non-yield

- ~~FR-231 residue — the namespaced `enum` prefix~~ — **FIXED, FR-232.** It
  was worse than reported: the FR-40 item graph was silently collapsing nodes
  (5 for 7 enums) because the enum path had drifted from the emitted-symbol
  contract `CSymbolNaming.h` exists to enforce.
- **FR-230's rounding looseness** — `int *p = malloc(10)` now backs
  `[i32; 3]`, so writing `p[2]` (UB in C) succeeds *silently* rather than
  trapping. No defined program changes behaviour, but the emitted crate can
  mask a heap overflow. The tightening is to bounds-check against the
  C-declared byte extent.
- **FR-162** — licensing. LICENSE is AGPL-3.0; 54 files carry Apache-2.0 WITH
  LLVM-exception. **Owner decision.**
- **The binding metric's denominator is inflated** — FR-228's runtime is ~44
  counted statements in every crate, so `statements` went 19695 → 32481 and
  `free/100 stmts` *fell* 11.71 → 7.10 with no emitter change. Both ratchet
  axes are absolute and unaffected; the report now says so inline. Excluding
  runtime items needs a change to the syn-based probe.
- **`_Exit` is almost free** — `process::exit` without the flush splice
  reproduces C exactly when stdout is a file, and fails only on a tty, where
  glibc line-buffers. Admitting it means the writer owning the terminal case.

## Rules this roadmap is built on

Each of these cost a wrong decision at least once, and they are the reason the
numbers above are hedged the way they are. `CLAUDE.md` holds the full set.

1. **The gate is not the goal.** One earlier session took the lit suite
   997 → 1040 across 21 FRs and 43 tests while TRACTOR moved 40 → 41.
2. **Count PROGRAMS, not events or sites.** A first-failure histogram has
   produced a wrong yield nine times.
3. **A strict-mode error is a first-failure reading of ONE case.**
   `emitrust-cc` halts at the first error, and twenty cases agreeing on the
   same first error are twenty first failures, not corroboration. Re-run with
   `--recover` and count the `unimplemented!` stubs.
4. **Clearing a stage is not passing.** Cross every blocker count with `kind`:
   a `lib` case that clears emit still needs an exportable symbol.
5. **A zero measured over one corpus is a claim about that corpus.**
6. **A refusal's wording is part of the ranking instrument.** The census keys
   on the diagnostic, so bespoke prose without a `RejectionLedger` needle
   drops a case into the `other` bucket. In FR-230 a missing needle did worse
   than hide a case — it *manufactured a lever that did not exist*.
7. **Verify a file:line anchor by running the tool, not by reading the line.**
   Six anchors were wrong or incomplete in one session; in each case the line
   existed and the case never reached it.
