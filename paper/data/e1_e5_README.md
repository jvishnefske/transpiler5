# E1 / E5 v3 — coloring precision and root-cause attribution

> **v3 note.** This file is v2's, kept for its definitions. What differs in v3:
>
> * HEAD is **`410643c57add76137056d1db9b32924150cddd1c`** (`410643c`,
>   *W5.12: FR-52 traits for external requirements*), in the worktree
>   `.claude/worktrees/agent-a4ffe4dae7c4ea8b1`, not `2fb6d54`.
> * `e1_e5_raw/collect.py` no longer assumes every `test/EndToEnd` case is a
>   single translation unit. It calls `../runline.py`, which derives each
>   case's inputs from the test's own lit `RUN:` line. See `README.md`.
> * Consequences: **344** projects measured (was 336), `e1_skipped.csv` is
>   **empty** (was 6 rows, all multi-TU EndToEnd cases), 1337 joined items
>   (was 1283), false_green 3 → 2, **false_red 0 → 0**.
> * Every number below that starts "v2" is superseded by `README.md`'s tables.


Re-run of the E1/E5 measurement in `../paper-data/`, at a newer commit. Column
schemas, corpus definitions, invocations, join key, and every definition
(`false green`, `false red`, the treatment of `declared`/`missing`) are
**unchanged** and are documented in `../paper-data/e1_e5_README.md`. Read that
file for the definitions; this file records the provenance and the new numbers.

## Provenance

- Repo `/home/j/src/transpiler5`, worktree
  `.claude/worktrees/agent-afa2558946f60fdd3`, HEAD
  **`2fb6d5467a391c56b69fa4fd665977d7917856cd`** (`2fb6d54`, `docs(design):
  FR-51 landed`). `git status --porcelain` was **empty** for the whole
  measurement: no source under `lib/`, `include/`, `tools/` and no `design.md`
  was modified. This is the same commit for all six experiments.
- Binary `build/bin/emitrust-cc`, CMake `Release`, LLVM/Clang/MLIR 21.1.8 from
  the flake dev shell.
- c-testsuite submodule pinned at `5c7275656d751de0e68b2d340a95b5681858ed07`
  (same pin as the prior run).
- Two invocations per project, both pure analysis (no `cargo build`, no
  `--search`), 12-way parallel, 120 s timeout. Nothing timed out. All 342
  projects (684 invocations) finished in 1.9 s wall clock.
- Driver: `e1_e5_raw/collect.py` + `e1_e5_raw/emit_e1_e5.py`. The prior run's
  scripts were not archived, so these are reimplementations written to the
  prior README's stated schemas; the 333 projects common to both runs come out
  **byte-identical**, which is the check that the reimplementation is faithful
  (see "What moved").

## Corpora

| corpus | prior | now | delta |
| --- | ---: | ---: | --- |
| `cpp-realworld` | 4 | **5** | `+ ringbuf-lib` (FR-51) |
| `c-realworld` | 13 | 13 | — |
| `endtoend` | 103 | **104** | `+ lib-crate-external-caller.c` (FR-51) |
| `c-testsuite` | 220 | 220 | — |
| total attempted | 340 | **342** | |
| measured | 333 | **336** | `argv-echo` is no longer skipped |
| skipped | 7 | **6** | |

## Headline results

```
corpus          projects  items  green yellow  red  ported stubbed dropped missing declared  FG  FR
cpp-realworld          5     43     37      0    6      33       5       5       0        0   0   0
c-realworld           13     33     33      0    0      18       8       1       6        0   1   0
endtoend              98    556    553      0    3     422       3       5      29       97   2   0
c-testsuite          220    651    651      0    0     609       0       0      26       16   0   0
TOTAL                336   1283   1274      0    9    1082      16      11      61      113   3   0
```

- **false red: 0** — 0 / 9 red predictions = **0.000 %**. The contract
  ("the probe never amputates an item the importer would have translated") still
  holds with zero exceptions, now over 1283 items in four corpora.
- **false green: 3** — 3 / 1274 green-or-yellow predictions = **0.235 %**;
  3 of the 11 dropped items (27.3 %) were unforeseen. The two prior ones are
  unchanged (`endtoend/recover-partial.cpp` `widen`, `_Complex double`;
  `endtoend/multi-tu-gate-g8-shared-header.c` `g`, pointer-typed global). The
  third is new and is a *measurement becoming possible*, not a regression — see
  below.
- `yellow` is still never predicted (0 / 1283). The Yellow rung remains
  unexercised by these corpora.
- Join rate is still 1.0000: 1283 coloring symbols, 1283 progress symbols,
  1283 matched, zero coloring-only and zero progress-only keys.
- `e1_unmatched.csv` is still exactly **15 off-graph** rows, same three
  projects (13 `cpp-realworld/shapes`, 1 `c-realworld/binary-tree`, 1
  `c-realworld/expr-eval`).
- `e1_missing_audit.csv` is still 61 rows, still **24 present-in-crate /
  37 absent-from-crate**.

Confusion matrix (`e1_confusion.csv`), prior values in parentheses:

```
predicted    ported    stubbed  dropped  missing  declared
green      1082(1062)   15(15)    3(2)    61(61)  113(113)
yellow          0(0)      0(0)     0(0)     0(0)     0(0)
red             0(0)      1(1)     8(8)     0(0)     0(0)
```

### E5

```
corpus         projects  with_rejects  rejected  root_items  ratio
cpp-realworld         5             2        23           8   2.88
c-realworld          13             4        11          11   1.00
endtoend             98             3         8           8   1.00
c-testsuite         220             0         0           0    n/a
TOTAL               336             9        42          27   1.56
```

`shapes` still carries the entire compression result: 19 rejected items over 5
direct blocker tags compress to 2 root tags and 4 root items, 4.75×. `polygon`
is still 4 rejections / 4 roots / 1.00×. The corpus-wide ratio moved 1.58 →
1.56 solely because `argv-echo` contributes one rejection whose root is itself.

## What moved, and why

`e1_rowdiff.py` diffs the two runs project by project. Over the **333 projects
present in both runs, ZERO rows changed** in `e1_summary.csv` and ZERO in
`e5_compression.csv`. Every difference between the runs is one of exactly three
new rows:

| project | why it is new | row |
| --- | --- | --- |
| `c-realworld/argv-echo` | previously **skipped** — its only item `main` is dropped for using `argv`, crate emission then hard-failed, and no `emitrust-progress.json` existed. FR-51 emits a library crate instead, so the item is now measurable. | 1 item, `c_main`, predicted `green reason=admissible`, actual `dropped root_blocker=argv` → **the third false green** |
| `cpp-realworld/ringbuf-lib` | new corpus project (FR-51) | 12 items, 12 green, 12 ported, 0 rejections |
| `endtoend/lib-crate-external-caller.c` | new corpus file (FR-51) | 8 items, 8 green, 8 ported, 0 rejections |

So the false-green count moving 2 → 3 is **not** a precision regression: the
probe's judgement on `argv-echo`'s `main` was always wrong, it simply could not
be scored before because the run produced no report at all. Anomaly 3 of the
prior README ("`c-realworld/argv-echo` yields no progress artifact at all") is
**closed**.

All other prior anomalies stand unchanged: yellow never predicted (1),
`missing` non-empty at 61 items with the same 24/37 audit split (2),
`c-testsuite` contributing zero rejections (4), the one red→stubbed item in
`shapes` (5), and 15 green→stubbed items (6).
