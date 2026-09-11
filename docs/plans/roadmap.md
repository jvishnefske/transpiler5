# TRACTOR roadmap

State at `7892c90`, 2026-09-10. Census re-run at 60/252. This is the human-readable companion to the
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
62 cases  0 items   ← never emitted a crate (48 are the openssl parse failures)
32        1         ← shallow; a bound worth trusting
13        2
 1        3
 4        4
16        7    16   8    16  14    32  15   ← the 80 SPHINCS+ lib cases
```

**It was blind to the stage that matters most; FR-236 fixed that.** The census
measured the EMIT stage only, so for any `lib` case a clean row meant nothing
on its own — that is rule 4, and item 3 above is what breaking it looks like.
It now reports a per-case **export verdict** over the 160 `lib` cases:

```
93 refused    64 exports    3 absent
```

calibrated against the scored run — **36/36 PASS read `exports`, 13/13
SYMBOL_MISSING read otherwise**. A disagreement there means the parser is
wrong, not the binary.

Two cautions that travel with it. The verdict is an **observation of what this
binary does today**, never a prediction: a walled case is not unreachable, it
needs an export-class fix *in addition* to the importer one. And **68 of the
160 verdicts rest on a case/underscore fold** between the Rust item name and
the C symbol, because no tool output carries a machine-readable map of the two
(the fold is used only where unique, and never to *declare* an export). Making
the refusal warning name the C symbol would delete the heuristic outright.

**The tool this repo ranks all work with still has no test of its own.** It is
not in the fast tier and never was — I had conflated it with `plan.py check`.
That gap is open.

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

### 3. FR-226 — the SPHINCS+ package · **68 cases walled, 12 payable behind ~5 fixes**

**This item and "FR-227, not to be funded" are the same wall, and I wrote them
as if they were independent.** Measured at the 60/252 census and re-measured
independently by FR-236: the 3-fix set clears **+34 EMIT** and **33 of the 34
are export-walled**. Those 34 are **4 symbols × 8 sha2 configurations**, and
their sha2 bodies write through a *mutable* byte slice. A mutable slice export
is FR-181's hard NO-GO (`&mut [T]` is `noalias` to LLVM where a C caller may
legally alias), and class 2 additionally demands a single-block, region-free,
constant-index, **read-only** body — so much as a `for` loop disqualifies it.
Probed: a read-only `for (i=0;i<32;i++) s+=b[i]` with a constant bound is
still refused.

This was the rule-4 trap ("clearing a stage is not passing") committed inside
the document that lists rule 4.

**The live remnant, and it exists because FR-233 was wrong about it — twice.**
I probed a body I invented rather than the corpus's. The real *blake*
`initialize_hash_function` is `{ (void)ctx; }` — a genuine no-op — so FR-226's
unaccessed-pointer class already exports it. Measured over FR-236's verdicts:
of the 80 SPHINCS+ `lib` cases, **exactly 12 export and 68 are refused**. The
12 are all blake `SPX_initialize_hash_function`, all matched *exact* rather
than through the name fold, all with `body_derived_risk: False`.

**Those 12 are not among the 34** — the 34 are sha2 only. The blake 12 sit at
**depth 6**: five distinct importer blockers, and crates reporting 19 refused
symbols against 3 exported. Since `tractor-eval.py` is strict-mode-only with no
partial credit, **all 19 must emit before any of the 12 scores.** All-or-
nothing, exactly what FR-221 measured when it fixed one blocker of a set and
got +0 — and depth is a lower bound, so "+12 after five fixes" is an upper
bound on an upper bound.

So FR-226's "+12" named the **right cases for the wrong reason**, and my
retraction was too broad: correct for sha2 and for `prf_addr`,
`gen_message_random` and `hash_message` in every backend, wrong for blake
`initialize_hash_function`. Three passes, each correcting the last.

### 3′. FR-234 — `argv`, and the `strtoX` family under it · **the export-aware cover's top pick**

Once export-walled cases leave the objective, the set-cover search picks a
**completely different trio** — argv, `pointer variable has no known target
object`, and `taking the address of a global variable` — worth **11 payable**
(exec 8, lib-exports 3) against the old trio's **1**. argv leads it.

#### Why argv, specifically · +5 EMIT upper bound, **all exec**

The new top lever, and the roadmap never ranked it. `exec` cases never meet the
export wall, so EMIT here can become PASS. **Upper bound, and the reason is
nameable**: all six have `main` dropped by recovery, so blockers inside `main`
are invisible — the census docstring names `004_nineality_sieve` as depth 1,
actually ≥4. Honest ceiling ≈ **+3**: 007 also needs `pow` (a deliberate
bit-exactness refusal), 006 a returned pointer, 008 an unreached item.

**Neither rung can be scored alone, measured 2026-09-10.** `--recover` on all
five argv programs drops `c_main` WHOLE — 1 rejected item, **0 stubs** — and in
each one `main` holds essentially the whole program (35–65 lines, 1–2 top-level
defs). So their depth is structurally invisible until argv is admitted, and the
"+3" above is a guess about code nothing has imported. `planArgvUsesFor` is
all-or-nothing and all five spell their uses as `strtoX(argv[i], &end, base)`,
so argv cannot admit until *both* the hosted-conversion argument position and
the `&end` out-parameter are admissible. Build rung 2 on plain locals (it will
score +0), widen argv in rung 3, **then re-measure** — do not re-forecast.
The one real bound is weak but genuine: 35–65 line programs cannot hide much,
unlike the SPHINCS+ cases where fourteen stubs sat behind one first error.

Smaller than it looks. `argv` is already half-built (`ArgvTableType`,
`ArgvArgOp`, a `Vec<Vec<i8>>` crate wrapper); `planArgvUsesFor` just admits a
narrow read grammar and otherwise leaves the old rejection. The real blocker is
underneath and independent: **`strtol` is not in the hosted table at all**, and
the `endptr` out-parameter is a second gap — both reproducible on a plain
`char buf[8]` with no argv in sight, so both are EndToEnd-testable with no
corpus dependency. `emitAtoiCall` is the shape to copy, `__emitrust_atof`
already implements the `strtod` prefix grammar, and FR-230's `strchr` cursor
bind is the `endptr` representation. Build it importer-first, argv second.

### ~~FR-228 — the stdout buffering model~~ · **DONE, EMIT +1 / PASS +0**

Landed 2026-09-10. Two things worth carrying forward. **A `BufWriter` does not
reproduce C** — glibc fills its buffer and flushes only when a write no longer
*fits*, then passes whole blocks through; measured at six boundaries, a
`BufWriter` matches one of them. And **`std::process::exit` runs no
destructors**, so every exit path needed an explicit flush spliced in
*unconditionally* — a shard cannot know whether it will be linked into a bin,
and guessing wrong truncates silently. 482 pre-existing goldens moved, all
verified to carry the runtime and re-checked by their own oracles.

### 5. The other two legs of the export-aware cover · 8 cases, both checked for well-definedness

Ranked by FR-236's export-aware search, and I checked the C before ranking
them — which changed the answer for one of the two.

**`taking the address of a global variable` — 024/025, *2* clean cases, not 4.**
I filed this as 4 and had to correct it: `025`'s `lib` half is **export-refused**
(`not all-scalar`), so it is +0 whatever the importer does, and `025`'s exec half
has three blockers (address-of-global, `errno`, and an `fgets` result test) not
one. The clean target is `024` + `024_lib`, both depth 1, the lib half exporting.
Well-defined C, no UB: a `static house_t the_house` with mutator functions
taking `&the_house`. This is the FR-62 C-owner-method shape almost exactly —
a singleton with methods — so the machinery to lift it already exists and
FR-179's actor path already handles singletons. The likeliest real work is
deciding the singleton's construction, not inventing a model.

**`pointer variable has no known target object` — 011/012, 4 cases (exec + lib).
Now the best ratio on the board: 4 cases behind ONE blocker.** All four are
depth 1, all four ship a real vector beside the skipped `has_ub` one, and both
`_lib` halves export (`driver`, `exact`, plain `no_mangle`). **Read the vectors
before touching it, though.** The C is
`char *data; printLine(data);` — an *uninitialized* pointer, which is UB, and
the refusal is CORRECT. But the corpus tags that path's vector `has_ub` and
the harness skips it, leaving one real vector that only exercises the
initialized path. So the case is winnable *without* modelling an indeterminate
pointer: since the `bad()` path is undefined, **a deterministic panic is a
legal refinement**, and the tree already does exactly that elsewhere ("C
undefined behavior refined into a deterministic panic"). What is NOT legal is
inventing a value for `data` and carrying on.

This is the counterexample to reading a blocker name and costing it: the two
legs look like one kind of work and are not.

### Not to be funded for yield

- **FR-230 item 3 — the pointer-member load.** +4 EMIT, **+0 PASS, and the +0
  is structural**: the only admitted pointer-struct-member model represents
  the field as an index into an owner array proven inside the unit, and every
  affected case is a `lib` entry point whose caller supplies a real address.
  **`020_stack_linked_list_lib` is the trap here, and it caught me.** It is
  depth 1, it has four real vectors, and it **exports** (`smallestValue`) — so
  by the two axes this document tells you to cross, blocker-count × `kind`
  × export, it reads as the best ratio on the board. It is worth zero, because
  this entry's `+0` is about whether the FIX IS POSSIBLE, which is a **third
  axis the census cannot see at all**. Cross all three before ranking
  anything.
- **FR-227 — the export wall.** 67 cases, 27% of the corpus — **and per FR-233
  it also gates the whole SPHINCS+ subtree, which is 128 of 252.** It is the
  single largest structural item on the board and the reason EMIT and PASS have
  decoupled. Not fundable as written, but it is where the corpus actually ends.
  Original entry: 67 cases,
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
