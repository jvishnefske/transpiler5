// FR-108's namespace prefix reached records, functions and globals but
// NEVER enums, and this file pins the missing fourth arm.
//
// The invariant: an enum's emitted symbol is namespace-qualified exactly
// as a record's is, and the cross-TU shape dedup is keyed on THAT symbol
// rather than on the bare C tag. Before this wave `namespace lib { enum
// Color { Red, Green }; }` emitted a bare `Color`, so a second, unrelated
// top-level `enum Color` collided with it and the whole -- entirely legal
// -- program was refused with "conflicting definition ... in another
// translation unit" when there was only ONE translation unit and nothing
// conflicting in it. Rejecting legal C++ is the visible symptom; the
// latent one is worse and is the same channel FR-108 closed for records:
// two same-shape enums in different namespaces would have MERGED silently
// into one Rust type.
//
// Pinned here because these are EMITTED SYMBOL NAMES, a byte-identity
// invariant shared with the FR-40 item graph (CSymbolNaming.h): the
// prefix composes outermost-to-innermost for nested namespaces, an
// anonymous namespace contributes the fixed `ns_anon_` tag, and a
// `LinkageSpecDecl` (`extern "C" { ... }`) is transparent and adds
// nothing. Both rename modes are pinned, because the fold order is
// load-bearing exactly as it is for `recordRustName`: the prefix goes on
// BEFORE the idiomatic camel fold, so `ns::Color` is `NsNsColor`
// (lint-clean) and never an `ns_ns_Color` that rustc's denied
// non_camel_case_types refuses.
//
// The VARIANT names deliberately take NO prefix: an enumerator lowers to
// an associated const of the tuple struct (`NsPE::A`), so the type that
// already carries the prefix is what separates two namespaces' identical
// enumerator spellings. `p::E{A}` beside `q::E{A}` is the case that
// proves it, and it is also a shape-DIFFERING pair, so it is exactly the
// program the old bare-name key refused.
// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RENAME
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=VARIANT
// RUN: emitrust-cc --emit=rust --namespace-modules %s | FileCheck %s --check-prefix=MOD
// RUN: emitrust-cc --emit=item-graph %s | FileCheck %s --check-prefix=GRAPH

extern "C" int printf(const char *, ...);

enum Color { Blue = 1, Black = 2 };

namespace ns {
enum Color { Red = 10, Green = 20 };
} // namespace ns

namespace a {
namespace b {
enum Depth { Two = 3 };
} // namespace b
} // namespace a

namespace {
enum Hidden { Shy = 4 };
} // namespace

namespace cfgns {
extern "C" {
enum Cfg { CfgOn = 5 };
}
} // namespace cfgns

namespace p {
enum E { A = 6 };
} // namespace p

namespace q {
enum E { A = 7 };
} // namespace q

int main(int argc, char **argv) {
  Color g = Blue;
  ns::Color n = ns::Red;
  a::b::Depth d = a::b::Two;
  Hidden h = Shy;
  cfgns::Cfg c = cfgns::CfgOn;
  p::E pe = p::A;
  q::E qe = q::A;
  printf("%d %d %d %d %d %d %d\n", (int)g, (int)n, (int)d, (int)h, (int)c,
         (int)pe, (int)qe);
  return 0;
}

// Rename OFF (`emitrust-import-c`): the prefix is the whole change and
// the C spelling of the tag and of every enumerator is untouched.
// CHECK-DAG: emitrust.enum_def @Color ["Blue", "Black"] [1, 2]
// CHECK-DAG: emitrust.enum_def @ns_ns_Color ["Red", "Green"] [10, 20]
// CHECK-DAG: emitrust.enum_def @ns_a_ns_b_Depth ["Two"] [3]
// CHECK-DAG: emitrust.enum_def @ns_anon_Hidden ["Shy"] [4]
// CHECK-DAG: emitrust.enum_def @ns_cfgns_Cfg ["CfgOn"] [5]
// Two namespaces, one enum spelling, one enumerator spelling: two
// definitions, not one merged type and not a rejection.
// CHECK-DAG: emitrust.enum_def @ns_p_E ["A"] [6]
// CHECK-DAG: emitrust.enum_def @ns_q_E ["A"] [7]

// Rename ON (`emitrust-cc`, FR-53): prefix first, camel fold second, so
// every emitted type name is already lint-clean CamelCase. The variant
// consts keep their SCREAMING_SNAKE fold and no prefix -- the type they
// hang off is what disambiguates them.
// RENAME-DAG: struct Color(u32);
// RENAME-DAG: struct NsNsColor(u32);
// RENAME-DAG: struct NsANsBDepth(u32);
// RENAME-DAG: struct NsAnonHidden(u32);
// RENAME-DAG: struct NsCfgnsCfg(u32);
// RENAME-DAG: struct NsPE(u32);
// RENAME-DAG: struct NsQE(u32);
// The variant pins take their own prefix: they are ORDERED (`impl NsPE`
// then `impl NsQE`) and would otherwise have to interleave with the
// unordered -DAG group above.
// VARIANT: impl NsPE {
// VARIANT-NEXT: const A: NsPE = NsPE(6);
// VARIANT: impl NsQE {
// VARIANT-NEXT: const A: NsQE = NsQE(7);

// FR-231 `--namespace-modules`: a namespaced enum lands INSIDE its `mod`,
// under its leaf name, exactly as a namespaced struct now does -- the
// emitter's FR-159 bucketing already accepts `emitrust.enum_def`, so the
// absolute-path symbol is the only thing that had to change.
// The crate-root use sites spell the ABSOLUTE path, type and variant
// const alike -- absolute for FR-159's own reason (a path written inside a
// `mod` is module-relative), and the root `::Color` stays unqualified.
// MOD:      let g: Color = Color::BLUE;
// MOD-NEXT: let n: crate::ns::Color = crate::ns::Color::RED;
// MOD-NEXT: let d: crate::a::b::Depth = crate::a::b::Depth::TWO;
// MOD-NEXT: let h: crate::anon::Hidden = crate::anon::Hidden::SHY;
// MOD:      let pe: crate::p::E = crate::p::E::A;
// MOD-NEXT: let qe: crate::q::E = crate::q::E::A;
// ...and the definitions land inside the module tree under their LEAF
// names, nested `mod` included.
// MOD:      mod ns {
// MOD:          pub(crate) struct Color(pub(crate) u32);
// MOD:      mod a {
// MOD-NEXT:     pub(crate) mod b {
// MOD:              pub(crate) struct Depth(pub(crate) u32);

// The FR-40 item graph recomputes every symbol from the AST ALONE, and
// CSymbolNaming.h's contract is that its node keys ARE the emitted item
// names, byte for byte -- duplicating a mangling there is how the two
// drift silently. `ItemGraphBuilder::enumSymbolFor` therefore goes through
// the same `enumRustName`, and this leg is the byte-for-byte pin: SEVEN
// enum nodes matching the seven `struct` lines above. Before this wave the
// graph produced FIVE, because `ns::Color` keyed onto `::Color` and
// `q::E` onto `p::E` -- two graph-key COLLISIONS, so the graph reported a
// program with fewer items than it has.
// GRAPH-DAG: node Color kind=enum def=1
// GRAPH-DAG: node NsNsColor kind=enum def=1
// GRAPH-DAG: node NsANsBDepth kind=enum def=1
// GRAPH-DAG: node NsAnonHidden kind=enum def=1
// GRAPH-DAG: node NsCfgnsCfg kind=enum def=1
// GRAPH-DAG: node NsPE kind=enum def=1
// GRAPH-DAG: node NsQE kind=enum def=1
