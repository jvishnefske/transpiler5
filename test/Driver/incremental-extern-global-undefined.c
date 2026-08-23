// FR-103: under --incremental, an extern global no TU defines costs the
// ITEMS that reference it, never the crate. The invariant this pins: the
// lwIP tcp_in.c/udp.c shape (`extern struct ip_globals ip_data;`, mutable
// AND address-taken via `&ip_data.member`) fails the FR-81 whole-value
// requirement arm for the WHOLE symbol, and before FR-103 the pending
// entry hard-failed in finalizeProject -- a module-finalization loop where
// no item was attributable, so the whole TU died even under --incremental,
// and it died EVEN when every referencing item had already been dropped
// (zero surviving IR uses). Now finalize attributes surviving uses back to
// their top-level functions and STUBS them (never erases -- erasure would
// orphan clean callers into the crate-fatal "function ... referenced but
// not defined" channel), and a pending entry with no surviving uses is
// forgiven outright (its users are already ledgered under their own
// blockers -- the dominant lwIP case).
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=NONE --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Recovery OFF is unchanged: a hard error, non-zero exit, no output
// directory. (Strict dies on the FIRST rejection, the returned member
// address -- the finalize-channel strict wording is pinned by the sibling
// incremental-extern-global-undefined-bin.c, whose input reaches finalize.)
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

struct G {
  int a;
  int b;
};
extern struct G g;

// The member address marks `g` address-taken (disqualifying the FR-81
// requirement arm for the whole symbol) and costs THIS item mid-import
// through the existing returned-pointer channel.
// WARN-DAG: :[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: returned pointer value (a member address is not a whole-global base) (recovered: item dropped)
int *addr_taker(void) { return &g.a; }

// The whole-value read is clean IR that SURVIVES to finalize; FR-103 stubs
// the reader there, located at the use, with the same wording strict mode
// raises as an error.
// WARN-DAG: :[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: extern global variable 'G' is referenced but not defined in any translation unit (recovered: emitted an unimplemented!() stub with the mapped signature)
int plain_reader(void) { return g.a; }

// The clean caller of the stubbed reader must PORT and call the stub --
// this is exactly why finalize stubs instead of erasing.
int caller(void) { return plain_reader() + 1; }

int untouched(int x) { return x + 2; }

// The zero-surviving-uses case: the ONLY user of `h` drops mid-import, so
// the pending entry reaches finalize with no IR uses at all and is
// forgiven -- no diagnostic beyond the user's own drop.
struct H {
  int c;
};
extern struct H h;
// WARN-DAG: :[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: returned pointer value (a member address is not a whole-global base) (recovered: item dropped)
int *h_taker(void) { return &h.c; }

int main(void) { return untouched(3); }

// No finalize diagnostic may mention the forgiven `h`, and the summary
// attributes the stub to the reader under its own tag.
// WARN-NOT: variable 'H'
// WARN: recovered 3 rejected top-level items:
// WARN-DAG: dropped 'addr_taker' [returned-pointer]
// WARN-DAG: dropped 'h_taker' [returned-pointer]
// WARN-DAG: stubbed 'plain_reader' [undefined-extern-global] unsupported: extern global variable 'G' is referenced but not defined in any translation unit

// Every translatable item reaches the crate; the reader's body is the
// standard recovery stub carrying the finalize wording.
// RUST-DAG: fn untouched(
// RUST-DAG: fn caller(
// RUST-DAG: fn c_main(
// RUST-DAG: fn plain_reader(
// RUST-DAG: unimplemented!("unsupported: extern global variable 'G' is referenced but not defined in any translation unit")

// The dropped items leave no trace, and no Externals trait may appear (a
// bin crate has no requirement route -- the trait arm was not sneaked in).
// NONE-NOT: addr_taker
// NONE-NOT: h_taker
// NONE-NOT: Externals

// The report names the construct that cost the reader.
// PORTING: | undefined-extern-global | 1 |

// JSON: { "tag": "undefined-extern-global", "count": 1 }

// STRICT: error: unsupported: returned pointer value (a member address is not a whole-global base)
