# TRACTOR roadmap

State at `411d987`, 2026-09-10. This is the human-readable companion to the
two authoritative files: `design.md` is the prose evidence ledger, and
`docs/plans/backlog.toml` is the machine index. **Where they disagree with
this document, they win** — this one summarises, they record.

## Where the score is

| | |
|---|---|
| **TRACTOR PASS** | **57 / 252** (exec 21, lib 36) |
| EMIT-cleared | 69 / 252 |
| SYMBOL_MISSING | 11 — emit clean, export refused |
| VACUOUS_PASS | 1 (`update_md5_lib`; its only vector is `has_ub` and skipped) |

One session moved this **41 → 57**: FR-224's libc shim table (+4), FR-229's
char-pointer byte view in two waves (+5, +5), FR-230's `alloca` (+2).

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

### 1. FR-230 item 2 — hosted `strchr` cursor bind · +1 PASS

`028_strchr` + its `_lib` twin. **The model is already proven byte-identical**
to the clang native on both corpus vectors and five adversarial inputs:
`__emitrust_strchr` already returns index-or-`-1`, exactly what a cursor bind
needs. The cost is not `strchr` itself but the two frontiers behind it — a
pointer assignment used as an rvalue/condition, and a possibly-null pointer
passed as an argument. `028_strchr_lib` is export-walled, so PASS is +1.

### 2. FR-229 residue — `scanf %f` · +2 PASS

Completes the byte-view family to 12/12 (`035`/`038` exec). FR-183
deliberately refused `%f` because glibc accepts `0x1p3`/`inf`/`nan(1)` and
Rust's `parse` does not — **check the corpus vectors before assuming the
narrow decimal case is enough.**

### 3. FR-226 — the SPHINCS+ package · up to +12 PASS, cost unknown

Five importer fixes plus the (already landed) unaccessed-pointer export class.
**Do not commit to the set up front.** Each of the 12 blake cases hides
fourteen stubbed functions, so "+12" is a loose upper bound. Fix **one** class,
re-measure the stub count, and re-cost — the histogram makes that cheap now.

### 4. FR-228 — the stdout buffering model · +1 PASS, wide blast radius

C's stdout is fully buffered off a terminal; Rust's is a `LineWriter`. They
agree on every normal termination and diverge on any that does not flush,
which is why `abort` is currently refused. The fix is a crate-wide
`BufWriter<Stdout>` flushed at normal exit only — **it shifts every emitted
byte of every crate that prints.** Fund it for correctness, not for the case.

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
