// FR-50/FR-40: `Field` versus `FieldIndirect` -- the one place the item graph
// distinguishes HOW a record reaches another, because the two answers have
// different consequences for FR-41's coloring.
//
// The rule, and it is a rule about the EMITTED RUST rather than about C: a
// field edge is `Field` when the emitted Rust field list SPELLS the target's
// name, and `FieldIndirect` when it does not. The importer's
// pointer-struct-member models (FR-35/37/38/39) erase a data-pointer member to
// an integer or an index, so `struct Atom *p` emits as `p: i64` and the target
// is never named; every other shape -- by value, an array of it, or a function
// prototype behind a function pointer -- keeps the name in the field list.
//
// That distinction is not cosmetic. FR-41 poisons a record Red through `Field`
// and NOT through `FieldIndirect`, so getting it wrong in the `FieldIndirect`
// direction manufactures false Reds, which FR-43's search can never recover
// from. See coloring-field-indirect.c for the colors and search-false-red.cpp
// for the end-to-end consequence.
//
// RUN: emitrust-cc --emit=item-graph %s -o - | FileCheck %s

struct Atom {
  int cell;
};

// By value: the emitted Rust is `struct ByValue { a: Atom, }`.
struct ByValue {
  struct Atom a;
};

// An array of it is still by value -- `[Atom; 3]` names `Atom`.
struct ByArray {
  struct Atom a[3];
};

// A data pointer is erased. So is a pointer to a pointer, and so is an array
// of pointers: no number of indirections puts the name back.
struct ByPointer {
  struct Atom *p;
};

struct ByPointerPointer {
  struct Atom **pp;
};

struct ByPointerArray {
  struct Atom *pa[4];
};

// A FUNCTION pointer is not a data pointer. `int (*)(struct Atom)` emits with
// its prototype intact, and the prototype spells `Atom`, so this is a `Field`
// edge despite the pointer in the way.
struct ByFunctionPointer {
  int (*fn)(struct Atom);
};

// Both routes at once. The by-value dependency is the real one and it
// SUBSUMES the indirect one: two edges between the same pair would say
// nothing a consumer could act on, and one dependency keeps one edge.
struct BothWays {
  struct Atom direct;
  struct Atom *indirect;
};

int main(void) { return 0; }

// CHECK:      node Atom
// CHECK:      edge BothWays -> Atom kind=Field{{$}}
// CHECK-NEXT: edge ByArray -> Atom kind=Field{{$}}
// CHECK-NEXT: edge ByFunctionPointer -> Atom kind=Field{{$}}
// CHECK-NEXT: edge ByPointer -> Atom kind=FieldIndirect{{$}}
// CHECK-NEXT: edge ByPointerArray -> Atom kind=FieldIndirect{{$}}
// CHECK-NEXT: edge ByPointerPointer -> Atom kind=FieldIndirect{{$}}
// CHECK-NEXT: edge ByValue -> Atom kind=Field{{$}}
// CHECK-NOT:  edge

// `BothWays` appears exactly once, and as the STRONGER kind.
// CHECK-NOT: edge BothWays -> Atom kind=FieldIndirect
