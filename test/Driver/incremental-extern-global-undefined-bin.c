// FR-103 companion: the NON-address-taken shape. A plain whole-value
// reader of an undefined extern struct is exactly what FR-81 admits as an
// Externals-trait requirement -- but only when the requirement route is
// open (a LIBRARY crate). A bin crate has no trait route, so the reader's
// uses survive to finalizeProject, and before FR-103 the whole TU died
// under --incremental despite containing zero unsupported constructs. This
// pins: (1) the bin-crate containment -- the reader is stubbed, its clean
// caller ports; (2) the finalize channel's STRICT wording, verbatim and
// still fatal (this input reaches finalize, unlike the address-taken
// sibling which strict-dies earlier); (3) the lib-crate differential --
// the SAME input as a library takes the FR-81 requirement route with no
// stub and no warning, proving containment fires only when the defer and
// trait arms have both passed.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Strict mode: the finalize-channel wording, use-located, hard error, no
// crate.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate
//
// Library crate: the FR-81 requirement route is untouched -- the reader
// goes generic over the Externals trait, nothing is stubbed, and the
// FR-103 wording appears nowhere.
// RUN: emitrust-cc --emit=crate --crate-type=lib --incremental %s \
// RUN:   -o %t.lib.crate 2>%t.lib.err
// RUN: FileCheck %s --check-prefix=LIB --input-file=%t.lib.crate/src/lib.rs
// RUN: FileCheck %s --check-prefix=LIBERR --allow-empty \
// RUN:   --input-file=%t.lib.err

struct G {
  int a;
  int b;
};
extern struct G g;

// STRICT: :[[#@LINE+2]]:{{[0-9]+}}: error: unsupported: extern global variable 'G' is referenced but not defined in any translation unit
// WARN: :[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: extern global variable 'G' is referenced but not defined in any translation unit (recovered: emitted an unimplemented!() stub with the mapped signature)
int plain_reader(void) { return g.a; }

int caller(void) { return plain_reader() + 1; }
int untouched(int x) { return x + 2; }
int main(void) { return untouched(3); }

// WARN: stubbed 'plain_reader' [undefined-extern-global]

// RUST-DAG: fn caller(
// RUST-DAG: fn untouched(
// RUST-DAG: fn c_main(
// RUST-DAG: unimplemented!("unsupported: extern global variable 'G' is referenced but not defined in any translation unit")

// JSON: { "tag": "undefined-extern-global", "count": 1 }

// LIB: pub trait Externals {
// LIB: fn plain_reader<E: Externals>(
// LIBERR-NOT: extern global variable
// LIBERR-NOT: unimplemented
