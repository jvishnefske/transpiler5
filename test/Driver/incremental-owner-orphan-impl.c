// FR-141: an owner METHOD may never outlive its owner STRUCT.
//
// The invariant this file pins: after a recovering import, no surviving
// `emitrust.method_of` may name a struct the module never defines. The
// emitted crate must be buildable or not written at all -- an `impl Foo`
// with no `struct Foo` is a crate that exits 0 and does not compile, the
// exact failure mode `rejection is a feature` exists to prevent.
//
// The defect, measured. The Phase-4 owner struct is created LAZILY, inside
// the owning function's body import, at the promoted array's DECLARATION
// statement (`emitOwnerLocal`); the methods that name it are created
// UNCONDITIONALLY from a plan `planOwners` fixed before any import ran. So
// whenever the owning function is rejected BEFORE its `char buf[N];` is
// reached, the methods are emitted and the struct is not. Here `make` is
// dropped at its SIGNATURE (`void *sink`), so its body is never entered at
// all; `--incremental` used to exit 0 having written
//     impl OwnerMakeBuf { fn tu0_fill(..) }
// with no `struct OwnerMakeBuf` anywhere, and `cargo build` said
// `error[E0425]: cannot find type 'OwnerMakeBuf' in this scope`. The
// dialect cannot catch this by design (EmitRustOps.td documents that
// `emitrust.impl`'s struct name is deliberately not cross-checked), so the
// guard is an end-of-TU module invariant in the importer.
//
// The C++ path has had the identical guard since FR-118 ("unsupported:
// method of an unimported class", pinned by
// test/Import/Cpp/cpp-rejected-class-no-trace.cpp); this is its C twin,
// stated as a sweep so it closes the whole class rather than one shape.
//
// The rejection is ATTRIBUTABLE: located at the method's own definition and
// naming the OWNING function, which is the FR-60 ranking signal -- port
// `make`'s blocker and you get `fill` back for free.
//
// REQUIRES: cargo
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --crate-type=lib \
// RUN:   --build 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// FR-43's frontier search must not be able to score a candidate state whose
// crate does not build either: the sweep lives in the import, so `--search`
// gets it for the same reason.
// RUN: emitrust-cc --emit=crate --incremental --search %s -o %t.searched \
// RUN:   --crate-type=lib --build 2>%t.search.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.search.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.searched/src/lib.rs
//
// Recovery OFF is unchanged, and that is what makes this change free: every
// shape that reaches the sweep already dies earlier in strict mode, so no
// golden, no byte-diff and no corpus ledger entry can move.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate --crate-type=lib 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

// The owner method. `fill`'s `char *s` unifies with `make`'s local `buf`,
// so Phase 4 promotes `buf` to `struct OwnerMakeBuf` and turns `fill` into
// one of its methods -- a decision taken before any import runs.
// WARN: :[[#@LINE+1]]:12: warning: unsupported: method of an owner struct that was never created: the owning function 'make' was rejected before its promoted local array 'buf' was imported (recovered: item dropped)
static int fill(char *s, long long v) {
  s[0] = (char)('0' + (v % 10));
  return 1;
}

// The owning function, rejected at its SIGNATURE: the body -- and with it
// the `char buf[32]` declaration that would have created the struct -- is
// never entered.
// STRICT: error: unsupported: void pointer parameter
void make(long long v, void *sink) {
  char buf[32];
  fill(buf, v);
  (void)sink;
}

// Both items are gone, and the driver says so.
// WARN: recovered 2 rejected top-level items:
// WARN: dropped 'make' [other] unsupported: void pointer parameter
// WARN: dropped 'tu0_fill' [owner-method-not-reached] unsupported: method of an owner struct that was never created: the owning function 'make' was rejected before its promoted local array 'buf' was imported

// Nothing dangling reaches the crate: no impl, and no reference to a type
// that was never defined. (`--build` above is the real oracle -- the crate
// this used to emit did not compile.)
// RUST-NOT: OwnerMakeBuf
// RUST-NOT: impl
// RUST-NOT: tu0_fill

// The progress artifact used to CONTRADICT the crate: `tu0_fill` read
// `status: missing`, blocker `unreached-by-import`, while its body sat in
// src/lib.rs. It is now honestly dropped, with its blame chain pointing at
// the owning function whose blocker is the thing to go fix.
// JSON: "missing": 0
// JSON: "symbol": "tu0_fill"
// JSON-NEXT: "kind": "function"
// JSON-NEXT: "status": "dropped"
// JSON: "blocker": "owner-method-not-reached"
// JSON: "attributed_via": "make"
// JSON-NEXT: "blame_chain": ["tu0_fill", "make"]
