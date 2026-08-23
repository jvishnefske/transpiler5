// FR-127: a W2.24 `planThrows` rejection is recovered per declaration,
// exactly as FR-53 recovers the other Pass-A planners.
//
// `planThrows` runs BEFORE the declaration walk and returned `failure()`
// straight out of `importTranslationUnit`, so one class-payload throw in a
// free function killed the whole translation unit under `--incremental`:
// no crate, no PORTING.md, no emitrust-progress.json -- the exact pre-FR-53
// failure mode `incremental-planner-rejection.c` pins, reintroduced by
// W2.24's pre-pass (measured on jsoncpp's json_value.cpp, whose namespaced
// `throwRuntimeError` killed an otherwise-translatable unit).
//
// The rejection is attributable: the offending throw sits in ONE definition,
// so that definition is credited, the plan is REBUILT without it (dropping a
// thrower changes the can-throw closure fixpoint, so the surviving plan must
// be the one a non-recovering run over the survivors would build), and
// everything else ports.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --incremental %s \
// RUN:   -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Recovery OFF is unchanged: the same diagnostic, as an ERROR, a non-zero
// exit, and no output directory -- `recoverFromRejections` is the only thing
// that switches the planner's behavior, which is what keeps the strict W2.24
// pins in Import/Cpp/exceptions-invalid.cpp untouched.
// RUN: not emitrust-cc --emit=crate --crate-type=lib %s \
// RUN:   -o %t.strict.crate 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

struct E {
  int code;
  E(int c) : code(c) {}
};

// The recovered warning must carry the planner's own location -- the throw
// keyword, where the non-recovering error points -- because that precision
// is what attributes the rejection to this one definition.
// WARN: :[[#@LINE+1]]:23: warning: unsupported: thrown exception payload must be a supported scalar type (recovered: emitted an unimplemented!() stub with the mapped signature)
void thrower(int c) { throw E(c); }

int keep(int x) { return x + 1; }

// The driver exits 0 and summarizes what it stubbed.
// WARN: recovered 1 rejected top-level item:
// WARN: stubbed 'thrower' [cxx-exception-payload] unsupported: thrown exception payload must be a supported scalar type

// `keep` and `E` reach the crate; `thrower` is a signature-only stub with
// the UNREWRITTEN signature (the surviving plan has an empty closure, so no
// carrier enum exists to rewrite it with) carrying the verbatim diagnostic.
// RUST-NOT: enum Throws
// RUST: pub fn thrower(_v0: i32) {
// RUST-NEXT: unimplemented!("unsupported: thrown exception payload must be a supported scalar type")
// RUST: pub fn keep(x: i32) -> i32 {
// RUST-NOT: enum Throws

// The report counts the survivors honestly: the FR-41 blame chain roots the
// stub at `exceptions`, the direct table keeps the planner's own tag.
// PORTING: **2 of 3 items ported (66.6%).**
// PORTING: | stubbed | 1 |
// PORTING: | root blocker | items |
// PORTING: | exceptions | 1 |
// PORTING: | stubbed | yellow | `thrower` | function | exceptions | thrower | cxx-exception-payload | unsupported: thrown exception payload must be a supported scalar type |

// JSON: "graph_items": 3
// JSON-NEXT: "ported": 2
// JSON-NEXT: "stubbed": 1
// JSON: { "tag": "cxx-exception-payload", "count": 1 }
// JSON: { "tag": "exceptions", "count": 1 }
// JSON: "symbol": "thrower"
// JSON: "status": "stubbed"

// STRICT: error: unsupported: thrown exception payload must be a supported scalar type
