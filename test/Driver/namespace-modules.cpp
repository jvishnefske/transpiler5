// FR-231 `--namespace-modules`, the emitted-spelling pin -- BOTH sides of the
// flag, in one file, over one source.
//
// The invariant this file exists to hold is the DEFAULT one: the flag is OFF
// unless asked for, and with it off every symbol keeps the historical
// `ns_<name>_`-per-level flattening byte for byte (the FLAT prefix below is
// the pre-FR-231 output verbatim). A flag whose default moved a byte of
// emitted Rust would be a behavior change wearing a feature's clothes, so the
// symmetry leg is pinned first and is the one that must never be relaxed.
//
// The MOD prefix pins what the flag buys, and each leg is a property that was
// separately verified against rustc rather than assumed:
//   * a single-level namespace becomes `mod geo`, and every use site spells
//     the ABSOLUTE path `crate::geo::twice` -- absolute because a path written
//     INSIDE a `mod` is module-relative, so `geo::twice` would be E0433 in an
//     item rendered in the module (FR-159's own reason);
//   * NESTING is the whole point of the flag (`ns_geo_ns_inner_thrice` is
//     exactly the spelling that degrades with depth) and renders as a real
//     TREE. `mod geo::inner {` -- which the pre-FR-231 renderer would have
//     written, since it printed the dotted path straight after `mod` -- is not
//     Rust at all. A nested `mod` is `pub(crate)`: the crate root spells
//     `crate::geo::inner::thrice`, and a private `mod inner` is E0603 there.
//     The OUTERMOST level stays a bare `mod`, which is what reproduces
//     FR-159's per-TU module bytes exactly;
//   * three levels deep composes with no special case;
//   * namespace REOPENING costs nothing and is pinned rather than built: the
//     emitter buckets by path STRING, so the second `namespace geo { }` block
//     lands in the same `mod geo` as the first;
//   * an ANONYMOUS namespace becomes `mod anon`. Rust has no anonymous
//     module, and one `mod anon` per TU matches C++'s one synthesized
//     internal-linkage entity however many `namespace { }` blocks reopen it;
//   * a namespaced record WITH METHODS keeps its `impl` at the crate ROOT,
//     spelled `impl crate::geo::Vec`. That is legal Rust (an inherent impl may
//     sit anywhere in the type's own crate) and it is why the flag needed no
//     module form for `emitrust.impl`;
//   * a namespace-free item (`flat`) is untouched by the flag.
//
// The IMPORTC leg pins the tool boundary: `emitrust-import-c` never sets the
// flag, so its goldens are unaffected by construction -- the same posture
// FR-53's `idiomaticRenameEnabled()` takes.
//
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=FLAT
// RUN: emitrust-cc --emit=rust --namespace-modules %s | FileCheck %s --check-prefix=MOD
// RUN: emitrust-import-c %s | FileCheck %s --check-prefix=IMPORTC

extern "C" int printf(const char *, ...);

namespace geo {
struct Vec {
  int x;
  int y;
  int len2() const { return x * x + y * y; }
};
int twice(int v) { return v * 2; }
namespace inner {
int thrice(int v) { return v * 3; }
} // namespace inner
} // namespace geo

int flat(int v) { return v + 1; }

// Reopened: this block must land in the SAME `mod geo` as the first one.
namespace geo {
int quad(int v) { return twice(twice(v)); }
} // namespace geo

namespace {
int hidden(int v) { return v - 1; }
} // namespace

namespace outer {
namespace mid {
namespace deep {
int five(int v) { return v + 5; }
} // namespace deep
} // namespace mid
} // namespace outer

int main(int argc, char **argv) {
  int seed = argc;
  geo::Vec v;
  v.x = seed * 3;
  v.y = seed * 4;
  printf("%d %d %d %d %d %d %d\n", v.len2(), geo::twice(seed),
         geo::inner::thrice(seed), geo::quad(seed), flat(seed), hidden(seed),
         outer::mid::deep::five(seed));
  return 0;
}

// The default: flattened, every item at the crate root, not one `mod`.
// FLAT:      struct NsGeoVec {
// FLAT:      fn ns_geo_twice(v: i32) -> i32 {
// FLAT:      fn ns_geo_ns_inner_thrice(v: i32) -> i32 {
// FLAT:      fn flat(v: i32) -> i32 {
// FLAT:      fn ns_geo_quad(v: i32) -> i32 {
// FLAT-NEXT:     let v0: i32 = ns_geo_twice(v);
// FLAT:      fn ns_anon_hidden(v: i32) -> i32 {
// FLAT:      fn ns_outer_ns_mid_ns_deep_five(v: i32) -> i32 {
// FLAT:      fn c_main(argc: i32) -> i32 {
// FLAT-NEXT:     let v: NsGeoVec = NsGeoVec { x: argc * 3i32, y: argc * 4i32, };
// FLAT:      impl NsGeoVec {
// FLAT-NOT:  mod

// Under the flag: paths at every use site, a real module tree at the end.
// MOD:       fn flat(v: i32) -> i32 {
// MOD:       fn c_main(argc: i32) -> i32 {
// MOD-NEXT:      let v: crate::geo::Vec = crate::geo::Vec { x: argc * 3i32, y: argc * 4i32, };
// MOD:           let v6: i32 = crate::geo::twice(argc);
// MOD-NEXT:      let v7: i32 = crate::geo::inner::thrice(argc);
// MOD-NEXT:      let v8: i32 = crate::geo::quad(argc);
// MOD-NEXT:      let v9: i32 = flat(argc);
// MOD-NEXT:      let v10: i32 = crate::anon::hidden(argc);
// MOD-NEXT:      let v11: i32 = crate::outer::mid::deep::five(argc);
// The record's methods stay at the crate root under a PATH self type.
// MOD:       impl crate::geo::Vec {
// MOD-NEXT:      fn len2(&self) -> i32 {
// MOD:       mod geo {
// MOD:           pub(crate) struct Vec {
// MOD:           pub(crate) fn twice(v: i32) -> i32 {
// Reopening: `quad` came from a SECOND `namespace geo` block and is here.
// MOD:           pub(crate) fn quad(v: i32) -> i32 {
// MOD-NEXT:          let v0: i32 = crate::geo::twice(v);
// MOD:           pub(crate) mod inner {
// MOD-NEXT:          pub(crate) fn thrice(v: i32) -> i32 {
// MOD:       mod anon {
// MOD-NEXT:      pub(crate) fn hidden(v: i32) -> i32 {
// MOD:       mod outer {
// MOD-NEXT:      pub(crate) mod mid {
// MOD-NEXT:          pub(crate) mod deep {
// MOD-NEXT:              pub(crate) fn five(v: i32) -> i32 {

// The importer tool never sets the flag: flattened, unconditionally.
// IMPORTC:   emitrust.struct_def @ns_geo_Vec
