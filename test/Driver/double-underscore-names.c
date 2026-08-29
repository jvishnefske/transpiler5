// FR-140: a legal C identifier carrying an INTERIOR double underscore must
// not produce a crate that fails its OWN lint table.
//
// `[lints.rust] non_snake_case = "deny"` (FR-53) is a tripwire on the
// emitter's codegen, and rustc's `is_snake_case` forbids a doubled
// underscore anywhere in the core of a name. `m__em` is perfectly legal C,
// the emitter preserves the C spelling verbatim, and the result was exit 0
// plus an UNBUILDABLE crate with no diagnostic at all:
//   error: structure field `g__global` should have a snake case name
//   error: method `f__unc` should have a snake case name
//   error: variable `a__b` should have a snake case name
//   error: variable `g__global_actor` should have a snake case name
// The rejection-is-a-feature policy says the emitter may not knowingly emit
// code its own manifest forbids.
//
// The fix ADDS `#[allow(non_snake_case)]` to the ENCLOSING ITEM of every
// name the emitter renders that would trip the lint -- it removes and
// renames nothing, so the worst a wrong answer can do is suppress one lint
// on one item; it can never miscompile. Placement is not free-choice: the
// attribute is IGNORED on a struct FIELD (measured -- the lint still
// fires), so a tripping field puts it on the STRUCT, and a tripping fn
// name, parameter or local puts it on the FN.
//
// Only `non_snake_case` is reachable this way, measured on this very
// program: type names go through `toUpperCamelCase`, which DROPS every
// underscore (`struct s__t` -> `ST`), so `non_camel_case_types` cannot
// see a `__`; and `non_upper_case_globals` only ever complains about
// LOWERCASE characters, so the SCREAMING_SNAKE global spelling
// (`TU0_C__ONST`) does not trip it either. Neither lint needs an allow.
//
// Collapsing the run instead (FR-73's fold, test/Import/C/
// static-underscore-prefix.c) is the better answer at a PREFIX boundary,
// where it cannot collide; collapsing an INTERIOR `__` can (`m__base` ->
// `m_base` against a C source that also spells `m_base`), so it is not
// done here.
//
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s
//
// The same question for an FR-52 requirement TRAIT, whose methods are
// declared under the C spellings that must satisfy them: a library crate
// with an undefined `host__scale` emitted
//   error: trait method `host__scale` should have a snake case name
// (measured on the built tool) and the allow goes on the TRAIT item.
// RUN: emitrust-cc --emit=crate --crate-type=lib \
// RUN:   %S/Inputs/double-underscore-externs.c -o %t.lib
// RUN: FileCheck --check-prefix=TRAIT %s < %t.lib/src/lib.rs

int g__global = 1;
int f__unc(int a__b) { int l__ocal = a__b + g__global; return l__ocal; }
struct s__t { int m__em; };
int use__it(void){ struct s__t v__ar; v__ar.m__em = f__unc(2); return v__ar.m__em; }
int main(void){ return use__it(); }

// The FR-62 actor struct holds the tripping field `g__global`: the allow
// goes on the STRUCT ITEM, and NOT on the field (where rustc ignores it) --
// the field line follows the struct header immediately.
// CHECK:      #[allow(non_snake_case)]
// CHECK-NEXT: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct GGlobalActor {
// CHECK-NEXT:     g__global: i32,

// Each tripping method carries its own allow: `f__unc` for both its name
// and its parameter `a__b`, `use__it` for its name and its local `v__ar`.
// The `impl` itself gets nothing -- the attribute is honoured on the fn.
// CHECK:      impl GGlobalActor {
// CHECK-NEXT:     #[allow(non_snake_case)]
// CHECK-NEXT:     fn f__unc(&mut self, a__b: i32) -> i32 {
// CHECK:          #[allow(non_snake_case)]
// CHECK-NEXT:     fn use__it(&mut self) -> i32 {

// The plain struct is covered the same way as the actor struct.
// CHECK:      #[allow(non_snake_case)]
// CHECK-NEXT: #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct ST {
// CHECK-NEXT:     m__em: i32,

// `c_main` is a clean name, but the local it binds for the actor is
// `g__global_actor`, which trips: the allow lands on the enclosing fn.
// CHECK:      #[allow(non_snake_case)]
// CHECK-NEXT: fn c_main() -> i32 {
// CHECK-NEXT:     let mut g__global_actor: GGlobalActor

// TRAIT:      #[allow(non_snake_case)]
// TRAIT-NEXT: pub trait Externals {
// TRAIT-NEXT:     fn host__scale(v0: i32) -> i32;
// The generic caller binds only generated `vN` names, so it needs nothing.
// TRAIT:      pub fn compute<E: Externals>(x: i32) -> i32 {
// TRAIT-NOT:  allow(non_snake_case)
