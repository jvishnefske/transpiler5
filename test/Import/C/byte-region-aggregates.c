// CTS-BR (00216): the u8-only BYTE-REGION aggregate model. An aggregate
// whose scalar leaves are ALL `unsigned char` (u8 members, u8 arrays,
// nested such structs/unions including unnamed union arms, and empty
// struct members contributing zero bytes) is padding-free by
// construction and imports as a byte region: the object is a plain
// `!emitrust.array<Nxui8>` where N == sizeof, NO struct_def is emitted
// for the record, constant initializers fold to complete byte images at
// import (APValue + target layout; holes keep the C99 zero fill), member
// access is a subscript at the member's constant byte offset,
// `(u8 *)&x` is a region base view, and `struct *` parameters over such
// types ride the existing region machinery. A union arm with non-u8
// leaves is tolerated TYPE-level only when its size equals the union's
// (the in6_addr shape: unsigned short u6_addr16[8] aliasing u8
// u6_addr8[16]); mixed-size non-u8 arms stay rejected (see
// byte-region-aggregates-invalid.c). A struct with a flexible array
// member is byte-region-representable with sizeof EXCLUDING the FAM;
// a static FAM-tail initializer folds into an EXTENDED image (this
// overturns the previous C99-17 declaration-site rejection — only
// runtime FAM-tail accesses reject now, see flexible-array-invalid.c).
// Int-leaf records stay on the typed struct path unchanged, and FAM /
// GNU zero-length members are tolerated (dropped, zero size
// contribution) there too when unaccessed.
//
// RUN: split-file %s %t
// RUN: emitrust-import-c %t/globals.c | FileCheck %s --check-prefix=GLOBALS --implicit-check-not=emitrust.struct_def
// RUN: emitrust-import-c %t/access.c | FileCheck %s --check-prefix=ACCESS --implicit-check-not=emitrust.struct_def
// RUN: emitrust-import-c %t/params.c | FileCheck %s --check-prefix=PARAMS --implicit-check-not=emitrust.struct_def
// RUN: emitrust-import-c %t/typedfam.c | FileCheck %s --check-prefix=TYPEDFAM

//--- globals.c
// File-scope byte images, spelled the way 00216 spells them. Every
// object below folds to a COMPLETE zero-filled byte image; the
// implicit-check-not proves no record type survives to the IR.
typedef unsigned char u8;

// An empty struct member contributes zero bytes: sizeof == 2.
typedef struct {} empty_s;
struct contains_empty {
    u8 a;
    empty_s empty;
    u8 b;
};
struct contains_empty ce = { { (1) }, (empty_s){}, 022, };
// GLOBALS-DAG: emitrust.global @ce <[1 : ui8, 18 : ui8]> : !emitrust.array<2xui8>

// An array of u8-only structs flattens to ONE region of n*sizeof bytes;
// the partial brace {1} and the bare 2 land at their layout offsets.
struct SS { u8 a[3], b; };
struct SS sinit16[] = { { 1 }, 2 };
// GLOBALS-DAG: emitrust.global @sinit16 <[1 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 2 : ui8, 0 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<8xui8>

// A compound-literal initializer folds like a brace list.
struct S { u8 a, b; u8 c[2]; };
struct S gs = ((struct S){1, 2, 3, 4});
// GLOBALS-DAG: emitrust.global @gs <[1 : ui8, 2 : ui8, 3 : ui8, 4 : ui8]> : !emitrust.array<4xui8>

// A string-literal member initializer contributes its bytes plus the
// C99 zero fill of the rest of the array member.
struct T { u8 s[16]; u8 a; };
struct T gt = {"hello", 42};
// GLOBALS-DAG: emitrust.global @gt <[104 : ui8, 101 : ui8, 108 : ui8, 108 : ui8, 111 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 42 : ui8]> : !emitrust.array<17xui8>

// A named u8-only union member is one shared byte at offset 0.
union UU { u8 a; u8 b; };
struct SU { union UU u; u8 c; };
struct SU gsu = {5, 6};
// GLOBALS-DAG: emitrust.global @gsu <[5 : ui8, 6 : ui8]> : !emitrust.array<2xui8>

// Unnamed union arms: all leaves u8, arms of DIFFERENT sizes (2-byte
// unnamed struct vs 4-byte struct S) — every arm is a view at offset 0
// of the sizeof(union)==4 region, and the bytes past the initialized
// arm keep the zero fill.
union UV {
    struct { u8 a, b; };
    struct S s;
};
union UV guv = {{6, 5}};
union UV guv2 = {{.b = 7, .a = 8}};
union UV guv3 = {.b = 8, .a = 7};
// GLOBALS-DAG: emitrust.global @guv <[6 : ui8, 5 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<4xui8>
// GLOBALS-DAG: emitrust.global @guv2 <[8 : ui8, 7 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<4xui8>
// GLOBALS-DAG: emitrust.global @guv3 <[7 : ui8, 8 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<4xui8>

// The in6_addr shape: the u16-array arm has NON-u8 leaves but exactly
// the union's 16-byte size, so it is tolerated as a type-level alias
// (accesses through it are not part of this contract); the region is
// the 16 bytes, initialized through the u8 arm.
struct in6_addr {
    union {
        u8 u6_addr8[16];
        unsigned short u6_addr16[8];
    } u;
};
struct pkthdr {
    struct in6_addr daddr, saddr;
};
struct pkthdr phdr = { { { 6, 5, 4, 3 } }, { { 9, 8, 7, 6 } } };
// GLOBALS-DAG: emitrust.global @phdr <[6 : ui8, 5 : ui8, 4 : ui8, 3 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 9 : ui8, 8 : ui8, 7 : ui8, 6 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<32xui8>

//--- access.c
// Member access = subscript at the member's constant byte offset (on
// locals directly, on globals through the staged whole-region copy);
// `(u8 *)&x` = region base view feeding the 00216 print_ walker;
// `&x.member` = base + offset.
typedef unsigned char u8;
struct S { u8 a, b; u8 c[2]; };
struct S gs = {1, 2, {3, 4}};
union UV {
    struct { u8 a, b; };
    struct S s;
};
union UV guv = {{6, 5}};
int printf(const char *, ...);

// The 00216 walker verbatim: a walked const u8* parameter is a byte
// slice (shared or mut is the importer's choice).
void print_(const char *name, const u8 *p, long size) {
  printf("%s:", name);
  while (size--) {
    printf(" %x", *p++);
  }
  printf("\n");
}
// ACCESS-LABEL: func.func @print_
// ACCESS-SAME: !emitrust.{{(mut_)?}}ref<!emitrust.slice<ui8>>

int members(void) {
  struct S ls = {9, 8, {7, 6}};
  ls.c[1] = ls.b;
  return gs.b + ls.c[1] + guv.s.c[0] + guv.a;
}
// ACCESS-LABEL: func.func @members
// ACCESS: %[[LS:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xui8>>
// ls.c[1] (offset 3) written from ls.b (offset 1):
// ACCESS-DAG: arith.constant 3 : i64
// ACCESS-DAG: arith.constant 1 : i64
// ACCESS-DAG: emitrust.subscript %[[LS]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<4xui8>>, i64) -> !emitrust.lvalue<ui8>
// The global read stages the whole region, then subscripts it.
// ACCESS-DAG: emitrust.global_load @gs : !emitrust.array<4xui8>
// guv.s.c[0]: the named struct arm resolves to byte offset 2 of the
// union region; guv.a to offset 0.
// ACCESS-DAG: arith.constant 2 : i64
// ACCESS-DAG: emitrust.cast %{{.*}} : ui8 to i32

void walks(void) {
  struct S ls = {9, 8, {7, 6}};
  print_("gs", (u8 *)&gs, sizeof gs);
  print_("ls", (u8 *)&ls, sizeof ls);
  print_("guv.b", (u8 *)&guv.b, sizeof guv.b);
}
// ACCESS-LABEL: func.func @walks
// ACCESS-DAG: emitrust.slice_of %{{.*}} -> !emitrust.{{(mut_)?}}ref<!emitrust.slice<ui8>>
// ACCESS-DAG: arith.constant 4 : i64
// The one-byte &guv.b view passes size 1.
// ACCESS-DAG: arith.constant 1 : i64
// ACCESS: call @print_
// ACCESS: call @print_
// ACCESS: call @print_

int main(void) {
  members();
  walks();
  return 0;
}

//--- params.c
// A `struct *` parameter over a u8-only aggregate is a byte-region
// pointer; member reads through it are region reads at constant
// offsets. The FAM struct W is byte-region-representable: sizeof(W)
// == 22 EXCLUDES the FAM, while the static FAM-tail initializer
// {1,2,3,4,5} folds into an EXTENDED 30-byte image (two 4-byte struct S
// elements, the second zero-filled past the 5).
typedef unsigned char u8;
struct S { u8 a, b; u8 c[2]; };
struct T { u8 s[16]; u8 a; };
struct V { struct S s; struct T t; u8 a; };
struct W {
  struct V t;
  struct S s[];
};
struct W gw = {{1, 2, 3, 4}, {1, 2, 3, 4, 5}};
// PARAMS-DAG: emitrust.global @gw <[1 : ui8, 2 : ui8, 3 : ui8, 4 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 0 : ui8, 1 : ui8, 2 : ui8, 3 : ui8, 4 : ui8, 5 : ui8, 0 : ui8, 0 : ui8, 0 : ui8]> : !emitrust.array<30xui8>
int printf(const char *, ...);

void walk(const u8 *p, long size) {
  while (size--) {
    printf(" %x", *p++);
  }
  printf("\n");
}

void foo(struct W *w) {
  struct S copy = w->t.s;
  struct T lt = ((const struct W *)w)->t.t;
  walk((u8 *)&copy, sizeof copy);
  walk((u8 *)&lt, sizeof lt);
  walk((u8 *)w, sizeof(struct W));
}
// PARAMS-LABEL: func.func @foo
// PARAMS-SAME: slice<ui8>
// The member copies land in local byte regions of the member sizes.
// PARAMS-DAG: emitrust.variable : !emitrust.lvalue<!emitrust.array<4xui8>>
// PARAMS-DAG: emitrust.variable : !emitrust.lvalue<!emitrust.array<17xui8>>
// sizeof(struct W) folds to the FAM-free 22.
// PARAMS-DAG: arith.constant 22 : i64
// PARAMS: call @walk
// PARAMS: call @walk
// PARAMS: call @walk

int main(void) {
  foo(&gw);
  return 0;
}

//--- typedfam.c
// Int-leaf records stay on the TYPED struct path — the byte-region
// model does not absorb them. FAM and GNU zero-length array members on
// typed records are TOLERATED when unaccessed: they contribute zero
// size and no field (the 00216 test_zero_init shape, with sub-level
// designators zero-filling every sibling).
struct SEA { int i; int j; int k; int l; };
struct SEB { struct SEA a; int r[1]; };
struct SEC { struct SEA a; int r[0]; };
struct SED { struct SEA a; int r[]; };
// TYPEDFAM-DAG: emitrust.struct_def @SEA ["i", "j", "k", "l"] [i32, i32, i32, i32]
// TYPEDFAM-DAG: emitrust.struct_def @SEB ["a", "r"] [!emitrust.struct<"SEA">, !emitrust.array<1xi32>]
// TYPEDFAM-DAG: emitrust.struct_def @SEC ["a"] [!emitrust.struct<"SEA">]
// TYPEDFAM-DAG: emitrust.struct_def @SED ["a"] [!emitrust.struct<"SEA">]

int printf(const char *, ...);

static void test_correct_filling(struct SEA *x) {
  if (x->i != 0 || x->j != 5 || x->k != 0 || x->l != 0)
    printf("wrong\n");
  else
    printf("okay\n");
}

int test_zero_init(void) {
  struct SEB b = { .a.j = 5 };
  struct SEC c = { .a.j = 5 };
  struct SED d = { .a.j = 5 };
  test_correct_filling(&b.a);
  test_correct_filling(&c.a);
  test_correct_filling(&d.a);
  return 0;
}
// TYPEDFAM-LABEL: func.func @test_zero_init
// TYPEDFAM: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"SEB">>
// TYPEDFAM: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"SEC">>
// TYPEDFAM: emitrust.variable : !emitrust.lvalue<!emitrust.struct<"SED">>
// TYPEDFAM: call @test_correct_filling
// TYPEDFAM: call @test_correct_filling
// TYPEDFAM: call @test_correct_filling

int main(void) { return test_zero_init(); }
