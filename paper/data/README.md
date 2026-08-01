# EmitRust paper measurement data — v3

v3 is a re-measurement of **E1, E2, E3, E5 and E6** after fixing a bug in the
measurement harness itself, not in the tool. `../paper-data-v2/` is left
untouched as the point of comparison, and every filename and column schema here
is identical to it.

**E4 was NOT re-run.** E4 measures only the `test/RealWorld` corpora over a
fixed set of historical revisions; it never touches `test/EndToEnd` and is
therefore unaffected by the bug. `e4_longitudinal.csv`,
`e4_longitudinal_plainmode.csv`, `e4_totals.csv`, `e4_totals_plainmode.csv`,
`e4_README.md`, `e4_corpus/` and `raw/` are **byte-for-byte copies of the v2
files**, and the numbers in them were measured at the commits named in
`e4_README.md`, not at this run's HEAD.

**Everything else in this directory was measured at**

```
410643c57add76137056d1db9b32924150cddd1c   (410643c)
W5.12: FR-52 traits for external requirements
```

in the worktree `/home/j/src/transpiler5/.claude/worktrees/agent-a4ffe4dae7c4ea8b1`,
with no file under `lib/`, `include/`, `tools/`, `test/` and no `design.md`
modified for measurement. c-testsuite submodule pinned at
`5c7275656d751de0e68b2d340a95b5681858ed07` (unchanged from v2).

---

## The bug: every EndToEnd case was measured as a single translation unit

v2 discovered the EndToEnd corpus like this:

```python
for p in sorted(list(e2e.glob("*.c")) + list(e2e.glob("*.cpp"))):
    ps.append(dict(corpus="endtoend", name=p.name, cwd=str(e2e),
                   inputs=[str(p)], tus=1))
```

Seven EndToEnd tests are deliberately **multi-TU**: their companion source
lives in `test/EndToEnd/Inputs/` (excluded from lit discovery by
`config.excludes`) and is named on the test's own `RUN:` line, e.g.

```
// RUN: emitrust-cc --emit=crate %s %S/Inputs/multi-tu-lib.c -o %t.crate --crate-name multi_tu --build
```

Handing `emitrust-cc` only `%s` makes symbols undefined that the project in
fact defines. `multi-tu.c` declares `extern int shared_counter` and calls
`add`, `lib_transform`; all three are defined in `Inputs/multi-tu-lib.c`.
v2 therefore recorded a whole-program `finalizeProject` failure —
*"'shared_counter' is referenced but not defined in any translation unit"* —
that is an artefact of the harness, not a property of the project.

### The fix

`runline.py` parses the **first `emitrust-cc` RUN line of each test** and uses
exactly the source files it names, resolving `%s` to the test file and `%S` to
`test/EndToEnd`; `tus` is set to the number of inputs found. Nothing is
hand-maintained, so the multi-TU set cannot drift from the tests. A test whose
RUN line cannot be parsed is written to `e2e_discovery_skipped.csv` with the
reason and is **never** silently downgraded to single-TU. Both harnesses —
`e2_e3_e6_raw/drive.py` (E2/E3/E6) and `e1_e5_raw/collect.py` (E1/E5) — import
the same function.

`e2e_discovery.csv` records what the parser resolved for all 105 cases.
`e2e_discovery_skipped.csv` is **empty (0 rows)**: all 105 parsed.

| | v2 | v3 |
| --- | ---: | ---: |
| EndToEnd cases discovered | 104 | **105** (`+ lib-crate-externals-trait.c`, added by FR-52) |
| cases with `tus > 1` | 0 | **7** |
| cases skipped as unparseable | n/a | **0** |

The seven, each `tus=2`:

| test | companion in `Inputs/` |
| --- | --- |
| `multi-tu.c` | `multi-tu-lib.c` |
| `multi-tu-gate-g1-fnptr-result-erasure.c` | `multi-tu-gate-g1-fnptr-result-erasure-e2e-other.c` |
| `multi-tu-gate-g3-owner-fallback.c` | `multi-tu-gate-g3-owner-fallback-e2e-other.c` |
| `multi-tu-gate-g5-region-api.c` | `multi-tu-gate-g5-region-api-other.c` |
| `multi-tu-gate-g6-cellslice-fn-external.c` | `multi-tu-gate-g6-cellslice-fn-external-e2e-other.c` |
| `multi-tu-gate-g8-ptr-global-external.c` | `multi-tu-gate-g8-ptr-global-external-e2e-other.c` |
| `multi-tu-gate-g8-shared-header.c` | `multi-tu-gate-g8-shared-header-def.c` |

The bug report named six; `multi-tu-gate-g8-shared-header.c` is a **seventh**.
It did not fail outright under v2's inputs — it emitted a crate from one TU —
so it never appeared in v2's skip lists, but it was measured with the wrong
input set all the same, and it is the one project whose E1 row moved.

Corroboration that the parsed input sets are the real ones: all nine
`multi-tu*` / `lib-crate*` lit tests **pass** at `410643c`
(`lit build/test/EndToEnd --filter 'multi-tu|lib-crate'` → 9/9).

### The 7 cases, before and after

| project | v2 strict / incr | v3 strict / incr | v3 ported/items |
| --- | --- | --- | ---: |
| `multi-tu.c` | 0 / 0 | **1 / 1** | 6/6 |
| `...g1-fnptr-result-erasure.c` | 0 / 0 | **1 / 1** | 7/7 |
| `...g3-owner-fallback.c` | 0 / 0 | **1 / 1** | 2/5 |
| `...g5-region-api.c` | 0 / 0 | **1 / 1** | 4/4 |
| `...g6-cellslice-fn-external.c` | 0 / 0 | **1 / 1** | 5/5 |
| `...g8-ptr-global-external.c` | 0 / 0 | **1 / 1** | 5/5 |
| `...g8-shared-header.c` | 0 / 1 | **1 / 1** | 4/4 |

With their real inputs all seven translate **under plain `--emit=crate`**, with
no `--incremental` and no `--search`. `probes` drops from 3 to 1 in every one.

---

## FR-52 is a separate, disjoint cause of movement

HEAD includes FR-52 (traits for external requirements), which changes how
unresolved externals behave in **library** crates. It contributes exactly two
things to this data, both **additive**:

* two new corpus entries, both added by `7838ebe`:
  `test/RealWorld/Cpp/Inputs/extern-plugin/` (cpp-realworld 5 → 6) and
  `test/EndToEnd/lib-crate-externals-trait.c` (endtoend 104 → 105);
* **no change to any pre-existing project.**

That last point is measured, not asserted. Per-project row diffs against v2,
restricted to the projects present in both runs:

| file | shared rows | changed | which |
| --- | ---: | ---: | --- |
| `e1_summary.csv` | 336 | **1** | `g8-shared-header.c` |
| `e5_compression.csv` | 336 | **1** | `g8-shared-header.c` |
| `e2_yield.csv` | 350 | **7** | the 7 multi-TU cases |
| `e3_search.csv` (probes/items/ported/builds) | 700 | **14** | the 7 multi-TU cases × 2 modes |
| `e6_cost.csv` (tus/nodes/edges only) | 350 | **7** | the 7 multi-TU cases |
| `e3_bound_sweep.csv` (probes/ported) | 70 | **0** | — |

**Every behavioural row that moved is a multi-TU EndToEnd case, i.e. the
discovery fix.** FR-52 moved nothing that existed before it; its whole
footprint is the two new rows. This is consistent with FR-52's own stated gate
that a project with no unresolved externals be byte-identical.

FR-52 notably did **not** absorb the three synthetic search fixtures. They
have a `main`, so they are binary crates, and FR-52 leaves a bin crate with
unresolved externals an error.

Machine-readable: `python3 e1_rowdiff.py`, `python3 e3_structdiff.py`,
`python3 e6_structdiff.py`, `python3 compare_old_new.py` (all rewired to
compare against `../paper-data-v2/`).

---

## E3 — the corrected result

`python3 e3_improved.py`

| | v2 | v3 |
| --- | ---: | ---: |
| projects | 350 | 352 |
| **improved by `--search`** | **9** | **3** |
| regressed by `--search` | 0 | **0** |
| of the improved, synthetic | 3 / 9 | **3 / 3** |
| `probes > 1` | 21 | 14 |
| `probes == 1` | 329/350 (94.0 %) | 338/352 (96.0 %) |

The three that survive are all purpose-built FR-43 fixtures under
`test/Project/`, each of which declares a symbol that no translation unit
defines *on purpose*, in order to construct the whole-program failure the
search exists to repair:

| project | effect | probes |
| --- | --- | ---: |
| `synthetic/search-backtrack.cpp` | no crate → crate, 4 items ported | 3 |
| `synthetic/search-bound.c` | no crate → crate, 2 items ported | 3 |
| `synthetic/search-determinism.c` | no crate → crate, 4 items ported | 7 |

`search-backtrack.cpp`'s own header comment says so: *"the header declares
`external_scale`, which the project calls and no translation unit defines"*.

The six EndToEnd `multi-tu*` cases v2 credited to the search are gone. The
search was repairing damage the harness had done — it was withdrawing the
items that referenced the symbols the harness had hidden, which is why its
`ported` counts there were *lower* than what plain strict translation achieves
with the correct inputs (e.g. `multi-tu.c`: search ported 1 of 6; strict now
ports 6 of 6).

**The honest finding: on this corpus the repair search improves nothing on
non-synthetic input. All three remaining improvements are on fixtures written
to exercise it.** It remains never-worse: across all 352 projects there is no
case where `mode=on` ports fewer items than `mode=off`, and none where `off`
builds and `on` does not.

Search overhead `on_wall / off_wall` is unchanged: median **1.17×**, mean
1.21×, p90 1.40×, max **4.35×** (`cpp-realworld/polygon`, 475.0 → 2063.9 ms;
v2 read 4.26× on the same project, 470.6 → 2003.0 ms).

### Bound sweep — `e3_bound_sweep.csv`

The sweep covers every project with `probes > 1`: **14** projects × 5 bounds =
70 rows (v2: 21 × 5 = 105). The 7 dropped are the multi-TU cases, which now
have `probes = 1`. **No shared row changed.** All 3 remaining rescues saturate
at `--max-search-nodes = 2`: bound 1 yields no crate, bound 2 yields the final
`ported`, bounds 4/8/16 buy nothing. `polygon` (probes 1→16, `ported` stuck at
4), `c-realworld/expr-eval` (probes 1→16, `ported` stuck at 0) and
`endtoend/recover-partial.cpp` (probes 1→8, `ported` stuck at 3) still burn the
whole budget for zero gain. The default bound of 8 is 4× larger than any
project in this corpus needs.

---

## E2 — the corrected result

`python3 e2_subset.py`

v2:

| corpus | n | strict_ok | strict_builds | incr_emitted | incr_builds | items | ported | ‰ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cpp-realworld | 5 | 3 | 3 | 5 | 5 | 43 | 33 | 767 |
| c-realworld | 13 | 9 | 9 | 13 | 13 | 33 | 18 | 545 |
| endtoend | 104 | 95 | 95 | 98 | 98 | 459 | 422 | 919 |
| c-testsuite | 220 | 220 | 220 | 220 | 220 | 635 | 609 | 959 |
| synthetic | 8 | 2 | 2 | 5 | 5 | 20 | 13 | 650 |
| **total** | **350** | **329** | **329** | **341** | **341** | 1190 | 1095 | 920 |

v3:

| corpus | n | strict_ok | strict_builds | incr_emitted | incr_builds | items | ported | ‰ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cpp-realworld | 6 | 4 | 4 | 6 | 6 | 50 | 40 | 800 |
| c-realworld | 13 | 9 | 9 | 13 | 13 | 33 | 18 | 545 |
| endtoend | **105** | **103** | **103** | **105** | **105** | 495 | 457 | 923 |
| c-testsuite | 220 | 220 | 220 | 220 | 220 | 635 | 609 | 959 |
| synthetic | 8 | 2 | 2 | 5 | 5 | 20 | 13 | 650 |
| **total** | **352** | **338** | **338** | **349** | **349** | 1233 | 1137 | 922 |

`strict_ok == strict_builds` and `incr_emitted == incr_builds` in **all 352
rows**: no emitted crate failed to compile anywhere, under either mode.

### The strict-vs-partial gap narrows sharply

Discriminating subset (cpp-realworld + c-realworld + endtoend, excluding the
saturated c-testsuite and the synthetic search fixtures):

| | n | strict | incremental | converted |
| --- | ---: | ---: | ---: | ---: |
| v2 | 122 | 107 (87.7 %) | 116 (95.1 %) | **9** |
| v3 | **124** | **116 (93.5 %)** | **124 (100.0 %)** | **8** |

Strict rises 5.8 points and incremental reaches 124/124. The gap between them
falls from **7.4 points (9 projects) to 6.5 points (8 projects)** — it narrows,
but only slightly, and every one of the eight conversions is a project that
was already a conversion in v2:

```
cpp-realworld/polygon        c-realworld/expr-eval
cpp-realworld/shapes         c-realworld/grep-lite
c-realworld/argv-echo        endtoend/incremental-builds.cpp
c-realworld/binary-tree      endtoend/recover-partial.cpp
```

**A correction to the bug report's premise.** The six hard-failing multi-TU
cases were *not* counted as partial-translation rescues in v2's `e2_yield.csv`:
they read `strict_ok=0, strict_builds=0, incr_emitted=0, incr_builds=0` — they
were failures in *both* columns and dragged both rates down. The one that v2
did count as a rescue is the seventh, `multi-tu-gate-g8-shared-header.c`
(`strict 0 → incr 1`); it is the project that leaves the conversion list, 9 → 8.
The "search rescue" framing applies to E3, where all six did appear.

The three remaining `incr_emitted = 0` projects in the whole corpus are the
three synthetic search fixtures.

---

## E1 / E5 — corrected

`e1_skipped.csv` is now **empty**. In v2 it held exactly six rows, all six the
hard-failing multi-TU cases, skipped because `--incremental` on half a project
wrote no `emitrust-progress.json` at all.

| | v2 | v3 |
| --- | ---: | ---: |
| projects measured | 336 | **344** |
| skipped | **6** | **0** |
| joined graph items | 1283 | 1337 |
| green / yellow / red | 1274 / 0 / 9 | 1328 / 0 / 9 |
| ported / stubbed / dropped / missing / declared | 1082 / 16 / 11 / 61 / 113 | 1124 / 15 / 10 / 64 / 124 |
| **false_red (red predicted, actually ported)** | **0** | **0** |
| false_green (green/yellow predicted, actually dropped) | 3 | **2** |
| unmatched (all off-graph) | 15 | 15 |

**The false-negative count is still zero.** `e1_confusion.csv` reads
`red → ported = 0` in both runs: the coloring never called an item red that the
translator then successfully ported. The one false-green that disappeared is
`multi-tu-gate-g8-shared-header.c`'s, which existed only because the harness
had hidden the definition of the global that item needed.

E5 (`e5_compression.csv`): the compression ratio `rejected / root_items` moves
1.56 → **1.60** on a larger denominator; the only shared row that changed is
`g8-shared-header.c`, whose 2 rejected items and 1 root tag go to 0.

---

## E6 — corrected

| | v2 (n=350) | v3 (n=352) |
| --- | ---: | ---: |
| Σ item-graph | 10.8 s | 11.0 s |
| Σ coloring | 10.6 s | 10.9 s |
| Σ `--incremental` | 14.9 s | 15.2 s |
| Σ `cargo build --release --offline` | 220.2 s | 227.2 s |
| tool as % of cargo | **6.8 %** | **6.7 %** |
| median `graph_ms / incremental_ms` | 0.76 | 0.76 |
| median `coloring_ms / incremental_ms` | 0.74 | 0.75 |
| median marginal translation cost | 27.4 % | 26.9 % |

Wall clock regressed on graph size (`nodes + edges`), all rows:

| mode | v2 slope (ms per node+edge) | v2 R² | v3 slope | v3 R² |
| --- | ---: | ---: | ---: | ---: |
| `graph_ms` | 0.0366 | 0.002 | 0.0505 | 0.004 |
| `coloring_ms` | 0.0526 | 0.004 | 0.0424 | 0.003 |
| `incremental_ms` | 0.3776 | 0.065 | 0.3190 | 0.044 |

Both analyses succeeded on **352/352** projects, including all 14 where strict
translation failed and all 3 where `--incremental` produced no crate. Neither
ever returned nonzero and neither ever produced an empty graph.

The `parse_floor_ms` caveat from `../paper-data/README.md` applies verbatim:
no `emitrust-cc` mode parses and then stops, so
`parse_floor_ms = min(graph_ms, coloring_ms)` is an **upper bound** on the
parse floor, not the floor.

---

## Build, toolchains, corpora

- `nix develop -c cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`, LLVM/Clang/MLIR
  21.1.8 from the flake dev shell. Binary `build/bin/emitrust-cc`.
- E1/E2/E3/E5/E6 `cargo build --release --offline`: **cargo 1.96.2 /
  rustc 1.96.1** from the nix dev shell (same as v2).
- E4 numbers are copied from v2 and were taken with **cargo 1.91.1** from
  `/usr/bin`; see `e4_README.md`.

| `corpus` | v2 n | v3 n | delta |
| --- | ---: | ---: | --- |
| `cpp-realworld` | 5 | **6** | `+ extern-plugin` (FR-52) |
| `c-realworld` | 13 | 13 | — |
| `endtoend` | 104 | **105** | `+ lib-crate-externals-trait.c` (FR-52) |
| `c-testsuite` | 220 | 220 | — |
| `synthetic` | 8 | 8 | — |
| **total** | **350** | **352** | |

**No sampling.** Every case in every corpus was run.

## Timing methodology and machine-load caveat

Unchanged from v2: best-of-3 consecutive runs per `emitrust-cc` timing, strictly
serial driver (one `emitrust-cc` or `cargo` at a time), wall clock around the
whole `subprocess.run`, output directories deleted between repetitions.
`cargo_ms` is a **single** run on a cold target directory.
`e6_cost_recheck.csv` is an independent best-of-5 re-run recording the per-row
load average.

The machine was **not** quiescent: same shared 20-core Linux workstation
(kernel 6.17, up 7 days) with a live desktop session. 1-minute load average
during the main serial sweep read 5.2–7.4; the `loadavg1` column of
`e6_cost_recheck.csv` reads **3.45–3.66** throughout, i.e. this run's recheck
was taken under a *lower* and much more stable load than v2's (7.59–20.11).
Cross-check: `polygon`'s search reads **2063.9 ms** in the best-of-3 main sweep,
**2050.0 ms** in the bound sweep at `--max-search-nodes=8`, and **2026.7 ms** in
the independent best-of-5 recheck — a 1.8 % spread — against v2's 2003.0 and
1977.1 ms for the same command. **Ratios between modes, measured
back-to-back, remain far more trustworthy than absolute milliseconds.** No
`ninja` build was running during any timed `emitrust-cc`.

## Files

| file | experiment | rows |
| --- | --- | ---: |
| `e2e_discovery.csv` | discovery fix — resolved inputs per EndToEnd case | 105 |
| `e2e_discovery_skipped.csv` | discovery fix — unparseable RUN lines | **0** |
| `e1_coloring_vs_outcome.csv` | E1 — one row per joined graph item | 1337 |
| `e1_summary.csv` | E1 — one row per measured project | 344 |
| `e1_skipped.csv` | E1 — projects with no parseable output | **0** |
| `e1_unmatched.csv` | E1 — off-graph / one-sided items | 15 |
| `e1_confusion.csv` | E1 — predicted × actual matrix | 3 |
| `e1_missing_audit.csv` | E1 — `missing` items vs. crate text | 64 |
| `e2_yield.csv` | E2 — partial-translation yield, per project | 352 |
| `e3_search.csv` | E3 — two rows per project (`mode=off`/`on`) | 704 |
| `e3_bound_sweep.csv` | E3 — `--max-search-nodes` ∈ {1,2,4,8,16} × 14 projects | 70 |
| `e4_*.csv` | E4 — **copied unchanged from v2, not re-run** | see `e4_README.md` |
| `e5_direct.csv` / `e5_root.csv` / `e5_compression.csv` | E5 — attribution | 18 / 15 / 344 |
| `e6_cost.csv` | E6 — analysis vs. translation cost | 352 |
| `e6_cost_recheck.csv` | E6 — best-of-5 re-run on the multi-file/synthetic projects | 27 |

Harness: `runline.py` (the fix), `e2_e3_e6_raw/` (E2/E3/E6: `drive.py`,
`synth.py`, `sweep.py`, `timing.py`, `emit_csv.py`), `e1_e5_raw/`
(E1/E5: `collect.py`, `emit_e1_e5.py`), plus the raw JSON each wrote.
Analysis helpers, not measurement data: `e3_improved.py`, `e2_subset.py`,
`compare_old_new.py`, `e1_rowdiff.py`, `e3_structdiff.py`, `e6_structdiff.py`.
