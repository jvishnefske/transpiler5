// FR-51, the measured defect that opened it: `--incremental` (FR-44) is
// DESIGNED to drop items outside the supported subset and still hand back a
// crate that builds plus a per-item account of what is missing. But when the
// dropped item happened to be `main`, crate emission then hard-failed --
// "cannot emit a crate: the input does not define a 'main' function" -- so the
// user got no crate, no PORTING.md, and no per-item numbers. That is exactly
// the all-or-nothing outcome the flag exists to remove, and it was reachable
// on a real corpus program (test/RealWorld/Inputs/argv-echo.c, whose `main`
// is dropped for using `argv`).
//
// The input below reproduces it minimally: `main` takes `argv`, which the
// importer cannot model, so the recovering import drops it and the module has
// no `c_main`. The project is now emitted as a LIBRARY crate, and the report
// is written.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: ls %t.crate | FileCheck %s --check-prefix=ARTIFACTS
// RUN: ls %t.crate/src | FileCheck %s --check-prefix=LAYOUT
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// The surviving library API really is exported, and the file-static really is
// not: dropping `main` must not change the visibility rule.
// RUN: FileCheck %s --check-prefix=LIB --input-file=%t.crate/src/lib.rs
//
// Without recovery the compile still fails at the unsupported construct, as
// it always did -- FR-51 changes the crate SHAPE, not what imports.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

int printf(const char *, ...);

static int width_of(int n) { return n < 10 ? 1 : 2; }

int describe(int n) { return width_of(n) * 100 + n; }

int main(int argc, char **argv) {
  printf("%d\n", describe(argc));
  printf("%s\n", argv[0]);
  return 0;
}

// The two FR-44 artifacts are present, beside a library crate.
// ARTIFACTS-DAG: Cargo.toml
// ARTIFACTS-DAG: PORTING.md
// ARTIFACTS-DAG: emitrust-progress.json

// LAYOUT-NOT: main.rs
// LAYOUT: lib.rs
// LAYOUT-NOT: main.rs

// WARN: warning: unsupported: use of main's argv parameter
// The ledger names `main` by its EMITTED symbol `c_main`, because that name
// is a join key against the FR-40 item graph; see `declLedgerName`.
// WARN: dropped 'c_main' [argv]

// `main` is the dropped item and the rest of the project ported, which is the
// whole point: the two items that ARE inside the subset are reported as
// ported instead of being lost with the crate.
// PORTING: # Porting status: `incremental_dropped_main`
// PORTING: | dropped | {{.*}} | `c_main` | function |
// PORTING-DAG: | ported | {{.*}} | `describe` | function |
// PORTING-DAG: | ported | {{.*}} | `tu0_width_of` | function |

// JSON: "graph_items": 3
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 0
// JSON-NEXT: "dropped": 1

// LIB: fn tu0_width_of(
// LIB: pub fn describe(
// LIB-NOT: fn main() { std::process::exit

// STRICT: error: unsupported: use of main's argv parameter
