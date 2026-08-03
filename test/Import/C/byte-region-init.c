// CTS-BR (00216): block-scope initialization of u8-only byte-region
// aggregates. Elements of an initializer list may be whole struct
// VALUES (a named local, a parenthesized name, a pointer deref), string
// literals, compound literals, and identity casts; bracing may be
// incomplete, superfluous, or parenthesized; GNU range designators
// (clang pre-expands them in the semantic form) may override earlier
// elements and carry runtime values. In every case the local is a
// `!emitrust.array<Nxui8>` region (N == sizeof): constant parts land as
// ui8 byte stores or folded images, embedded struct values as region
// copies at their byte offsets, and runtime scalars as int-to-ui8 casts
// stored at their offsets. No struct_def survives to the IR
// (implicit-check-not).
//
// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not=emitrust.struct_def

typedef unsigned char u8;
struct S { u8 a, b; u8 c[2]; };
struct T { u8 s[16]; u8 a; };
struct U { u8 a; struct S s; u8 b; struct T t; };

// Struct-value elements: a named local, its parenthesized spelling, a
// deref of a pointer to it, and the incomplete-bracing form. Each lu*
// is the 23-byte U region; ls's 4 bytes are copied to offset 1.
int elements(void) {
  struct S ls = {1, 2, 3, 4};
  const struct S *pls = &ls;
  struct U lu1 = {3, ls, 4, {"huhu", 43}};
  struct U lu2 = {3, (ls), 4, {"huhu", 43}};
  struct U lu22 = {3, *pls, 4, {"huhu", 43}};
  struct U lu21 = {3, ls, 4, "huhu", 43};
  return lu1.b + lu2.b + lu22.b + lu21.b;
}
// CHECK-LABEL: func.func @elements
// CHECK: emitrust.variable named "ls" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK-DAG: emitrust.variable named "lu1" : !emitrust.lvalue<!emitrust.array<23xui8>>
// "huhu" bytes and the trailing 43 land inside the U regions.
// CHECK-DAG: 104 : ui8
// CHECK-DAG: 117 : ui8
// CHECK-DAG: 43 : ui8
// U.b reads are subscripts at byte offset 5.
// CHECK-DAG: arith.constant 5 : i64

// Copy-shaped initializers: from a deref, from an identity cast, and
// from a plain value — all whole-region copies into fresh 4-byte
// regions.
int copies(void) {
  struct S x = {1, 2, {3, 4}};
  const struct S *pls = &x;
  struct S y = *pls;
  struct S z = (struct S)x;
  struct S w = x;
  return y.a + z.b + w.c[0];
}
// CHECK-LABEL: func.func @copies
// CHECK: %[[X:.*]] = emitrust.variable named "x" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK-DAG: %[[Y:.*]] = emitrust.variable named "y" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK-DAG: %[[Z:.*]] = emitrust.variable named "z" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK-DAG: %[[W:.*]] = emitrust.variable named "w" : !emitrust.lvalue<!emitrust.array<4xui8>>

// String-literal members, compound literals in initializer position,
// superfluous braces, a hole (lu4 leaves U.s.c[1] out), and useless
// parens around values: all fold into the same region shapes.
int literals(void) {
  struct T lt = {"hello", 42};
  struct S cl = (struct S){7, 8, {9, 10}};
  struct U lu3 = { 3, {5, 6, 7, 8,}, 4, {"huhu", 43}};
  struct U lu4 = { 3, {5, 6, 7,}, 5, { "bla", 44} };
  struct S ls3 = { (1), (2), {(((3))), 4}};
  return lt.a + cl.a + lu3.a + lu4.a + ls3.a;
}
// CHECK-LABEL: func.func @literals
// CHECK-DAG: emitrust.variable named "lt" : !emitrust.lvalue<!emitrust.array<17xui8>>
// CHECK-DAG: emitrust.variable named "cl" : !emitrust.lvalue<!emitrust.array<4xui8>>
// CHECK-DAG: emitrust.variable named "lu3" : !emitrust.lvalue<!emitrust.array<23xui8>>
// "hello" and "bla" bytes appear as ui8 stores or folded images.
// CHECK-DAG: 111 : ui8
// CHECK-DAG: 98 : ui8
// CHECK-DAG: 42 : ui8

// GNU range designators with runtime values: clang pre-expands the
// ranges, later ranges override earlier ones ([4...7] wins over both
// neighbors on the overlap), and the runtime `elt` flows through an
// int-to-byte cast into the region.
int ranges(void) {
  int elt = 0x42;
  struct T lt2 = { { [1 ... 5] = 9, [6 ... 10] = elt, [4 ... 7] = elt + 1 }, 1 };
  return lt2.a;
}
// CHECK-LABEL: func.func @ranges
// CHECK: emitrust.variable named "lt2" : !emitrust.lvalue<!emitrust.array<17xui8>>
// CHECK-DAG: 9 : ui8
// CHECK-DAG: emitrust.cast %{{.*}} : i32 to ui8

int main(void) {
  return elements() + copies() + literals() + ranges();
}
