# E4 — Longitudinal capability measurement (NOT re-run for v3)

> **v3 note. E4 was not re-measured.** E4 uses only the `test/RealWorld`
> corpora over a fixed set of historical revisions; it never reads
> `test/EndToEnd`, so the single-translation-unit discovery bug fixed in v3
> could not have touched it. `e4_longitudinal.csv`,
> `e4_longitudinal_plainmode.csv`, `e4_totals.csv`, `e4_totals_plainmode.csv`,
> `e4_corpus/`, `raw/` and the `e4_*.py` scripts in this directory are
> **byte-for-byte copies of the v2 files**. The numbers below were taken at the
> commits named in this file, NOT at v3's HEAD `410643c`.


This is a **re-run** of the E4 measurement in `../paper-data/`, at a newer
commit, with **one revision added at the end** (order 8, `4c89af1`, FR-51) and
the frozen benchmark re-extracted from the new head. Everything else — the
design, the discovery rules, the invocation, the crate-build step, the
empty-versus-zero convention — is unchanged and is documented in
`../paper-data/e4_README.md`. Read that file for the rationale; this file
records only what is different and what the numbers now are.

Files, all prefixed `e4_`:

| file | contents |
| --- | --- |
| `e4_longitudinal.csv` | primary per-project series (Series A + Series B), 8 revisions × 18 projects |
| `e4_totals.csv` | primary per-commit totals, rows for `c`, `cpp`, `all` |
| `e4_longitudinal_plainmode.csv` | control: `--emit=crate` (no `--incremental`) at **every** revision |
| `e4_totals_plainmode.csv` | totals for the control series |
| `e4_corpus/` | the frozen benchmark inputs, re-extracted (see below) |
| `e4_measure.py` | harness (unchanged except for a raw-JSON-only `crate_kind` field) |
| `e4_aggregate.py` | raw JSON → CSV (unchanged except for the 8th revision) |
| `e4_libtally.py` | per-revision lib-vs-bin split, read from the raw JSON |
| `raw/e4_<order>_<sha>_<mode>.json` | per-run raw records |

## THE BENCHMARK GREW BY ONE PROJECT — read this before comparing runs

The frozen corpus must come from the newest commit in the range, and the newest
commit now contains `test/RealWorld/Cpp/Inputs/ringbuf-lib/` (3 TUs, 2 headers,
no `main`), added by FR-51. So:

* prior run: **17 projects** (13 C + 4 C++), corpus extracted at `bc0be94`;
* this run: **18 projects** (13 C + 5 C++), corpus extracted at `2fb6d54`.

A `diff -rq` of the two `e4_corpus/` trees reports exactly one difference:
`ringbuf-lib` is present only in the new one. Every other byte of the benchmark
is identical, so the two series are comparable **provided the denominators are
quoted**. `16/17` (prior, order 7) and any `n/18` here are NOT the same
fraction. **A reader must not read the larger denominator as progress.**

`test/RealWorld` is byte-identical at `4c89af1` and at `2fb6d54`; `2fb6d54` is
a docs-only commit. The corpus is therefore the FR-51 corpus either way.
`e4_corpus/CORPUS_SOURCE_COMMIT.txt` records this.

## Revisions

Orders 1–7 are the prior seven, unchanged, so the series is comparable. Order 8
is new.

| order | sha | subject | mode_used (Series A) |
| --- | --- | --- | --- |
| 1 | `54fe0fe` | test(EndToEnd): widen C++ differential net | `--emit=crate` |
| 2 | `c3fbb13` | W5.0: FR-46 C++ RealWorld demand corpus | `--emit=crate` |
| 3 | `1f1dc8f` | W5.7: FR-47 implicit-`this` method-call receiver | `--emit=crate` |
| 4 | `385efc3` | W5.4: FR-44 incremental crate output | `--emit=crate --incremental` |
| 5 | `1a37f6c` | W5.3: FR-43 frontier tree search | `--emit=crate --incremental` |
| 6 | `c665b20` | W5.9: FR-48 C++ reference types | `--emit=crate --incremental` |
| 7 | `bc0be94` | W5.10: FR-50 `--search` never loses | `--emit=crate --incremental` |
| **8** | **`4c89af1`** | **W5.11: FR-51 library crates** | `--emit=crate --incremental` |

Each revision is checked out in an **isolated clone** of the repository
(`scratchpad/e4repo`), never in the main checkout and never in this agent's
worktree; `build/` is removed, CMake reconfigured, and only the `emitrust-cc`
target built.

## `LIB_BUILT` is not `crate_builds`, and neither is folded into the other

FR-51 introduced the harness outcome `LIB_BUILT`: the project has no `main`, so
`emitrust-cc` emits `src/lib.rs` plus a `[lib]` manifest. E4's own columns are
`crate_emitted` / `crate_builds` and have always been statements about
**compilation**, not about behaviour, so a library crate that compiles is a
legitimate `crate_builds = 1`. What it is *not* is evidence that the
translation is correct: a library has no entry point, cannot be run, and cannot
be diffed against a native build.

Therefore `e4_libtally.py` reports, per revision, how many of the emitted crates
were `lib` and how many were `bin`, read from the raw JSON's `crate_kind`
field. **The two are never summed into a single success number in any
narrative built on this data.** The CSV schema itself is deliberately
unchanged, so that `e4_totals.csv` can be diffed against the prior run column
for column.

## Result — primary series (`e4_totals.csv`, `all` rows)

All 8 revision builds succeeded. No project regressed on any metric at any
step, in either series (`e4_rowdiff.py` reports **0 monotonicity violations**).

| order | sha | crates emitted | crates building | of which lib | ported / graph_items |
| --- | --- | --- | --- | ---: | --- |
| 1 | `54fe0fe` | 10/18 | 10/18 | 0 | (undefined) |
| 2 | `c3fbb13` | 10/18 | 10/18 | 0 | (undefined) |
| 3 | `1f1dc8f` | 11/18 | 11/18 | 0 | (undefined) |
| 4 | `385efc3` | 16/18 | 16/18 | 0 | 37/63 |
| 5 | `1a37f6c` | 16/18 | 16/18 | 0 | 37/63 |
| 6 | `c665b20` | 16/18 | 16/18 | 0 | 39/63 |
| 7 | `bc0be94` | 16/18 | 16/18 | 0 | 39/63 |
| **8** | **`4c89af1`** | **18/18** | **18/18** | **2** | **51/76** |

Prior-run denominator for orders 1–7 was **17**, not 18: the same numerators
(10, 10, 11, 16, 16, 16, 16) over a benchmark one project smaller. The two
projects `ringbuf-lib` and `argv-echo` are the ones that move at order 8, and
they are precisely the two library crates. **`ringbuf-lib` scores
`crate_emitted = 0` at every one of orders 1–7** — it has no `main`, so before
FR-51 the emitter hard-failed on it exactly as it did on `argv-echo`.

Control series (`e4_totals_plainmode.csv`, plain `--emit=crate` at every
revision): 10, 10, 11, 11, 11, 11, 11, **12** out of 18. The single step at
order 8 is `ringbuf-lib`. `argv-echo` stays at 0 in the control, because
without `--incremental` its `argv` blocker is still a hard error rather than a
recovered drop — FR-51 removed the *no-`main`* failure, not the blocker.

Per-corpus at order 8: C **13/13** emitted and building (`graph_items` 33,
`ported` 18); C++ **5/5** emitted and building (`graph_items` 43, `ported` 33).

The `graph_items` denominator moves 63 → 76 at order 8, for the first time in
the series. It is not a measurement change: +12 is `ringbuf-lib`'s twelve items
becoming measurable, +1 is `argv-echo`'s single item. Numerator +12 (39 → 51)
is `ringbuf-lib` porting 12/12; `argv-echo` contributes 0/1. **Any per-item
percentage across orders 7 and 8 is therefore over different denominators and
must be quoted as `39/63` and `51/76`, never as 62 % → 67 %.**

## What moved relative to the prior run

`e4_rowdiff.py` diffs both series row by row on
`(order, corpus, project)`:

| series | shared rows | changed | new rows |
| --- | ---: | ---: | ---: |
| `e4_longitudinal.csv` | 119 | **0** | 25 |
| `e4_longitudinal_plainmode.csv` | 119 | **0** | 25 |

Zero changed rows: the seven historical revisions reproduce the prior run
**exactly**, project for project and column for column, even though they were
rebuilt from scratch in a different clone. The 25 new rows per series are
7 × `cpp/ringbuf-lib` (orders 1–7, all `0/0`) plus the 18 rows of order 8.

## Validity check

At order 8 the harness independently reproduces the repository's own checked-in
per-item ledgers: `fixed-stats 11/11`, `polygon 4/8`, `ringbuf-lib 12/12`,
`shapes 1/7`, `tokenizer 5/5` — including the `ringbuf-lib 12/12` that
`test/RealWorld/Cpp/expected-outcomes.txt` records for its `LIB_BUILT` entry.
No per-commit expectation file was consulted by the harness itself.

## Toolchain note

`cargo build --release --offline` uses **cargo 1.91.1** (`/usr/bin/cargo`), the
same toolchain the prior E4 run used, so this series stays internally
comparable. The other five experiments use the nix dev shell's cargo 1.96.2, as
their prior run did.

