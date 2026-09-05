// FR-195, the REPORT half. Under `--incremental` the FR-53 case fold makes
// `Compute` and `compute` compose one emitted symbol; the loser used to be
// ledgered as `dropped 'tu0_compute'` -- the SURVIVOR's symbol, and the
// FR-40 item-graph key the survivor answers to. So the report said the
// item `tu0_compute` had been dropped while the emitted crate contained it,
// called it, and executed it: a report about the wrong item, on top of the
// silent call-site rebind FR-195 closes.
//
// Now the loser is stubbed under a symbol RESERVED for it, so it joins the
// off-graph rejection table under that spelling and the survivor's graph
// node stays honestly `ported`. `--incremental` still hands back a crate
// that builds, which is the whole flag's contract.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
//
// Without recovery the compile still fails at the collision, located, with
// the wording it always had -- FR-195 changes recovery, nothing else.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

int printf(const char *, ...);

static int Compute(int x) { return x * 3 + 1; }
static int compute(int x) { return x * 5 + 2; }

int describe(int n) { return Compute(n) + compute(n); }

int main(void) {
  printf("%d\n", describe(2));
  return 0;
}

// WARN: warning: unsupported: function 'compute' emits as 'tu0_compute', which collides with 'Compute' (the idiomatic rename folds both spellings onto one symbol) (recovered: emitted an unimplemented!() stub with the mapped signature)
// WARN: stubbed 'tu0_compute_collision1' [other] unsupported: function 'compute' emits as 'tu0_compute', which collides with 'Compute' (the idiomatic rename folds both spellings onto one symbol)

// The survivor is a PORTED graph item, and the reserved symbol is what the
// off-graph rejection table names.
// PORTING: | ported | green | `tu0_compute` | function |
// PORTING: ## Rejected items outside the item graph
// PORTING: | stubbed | yellow | `tu0_compute_collision1` |

// JSON: "symbol": "tu0_compute",
// JSON-NEXT: "kind": "function",
// JSON-NEXT: "status": "ported",
// JSON: "off_graph_items"
// JSON: "symbol": "tu0_compute_collision1",

// The caller reaches the survivor and the stub as two DIFFERENT items --
// this is the line that used to read `tu0_compute(...) + tu0_compute(...)`.
// RUST: fn describe(n: i32) -> i32 {
// RUST: = tu0_compute(
// RUST-NEXT: = tu0_compute_collision1(

// STRICT: error: unsupported: function 'compute' emits as 'tu0_compute', which collides with 'Compute' (the idiomatic rename folds both spellings onto one symbol)
