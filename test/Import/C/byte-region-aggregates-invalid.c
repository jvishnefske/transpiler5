// CTS-BR (00216) negative space. The byte-region admission is bounded
// to aggregates whose scalar leaves are ALL unsigned char: (1) a byte
// view `(u8 *)&x` of an aggregate with non-u8 leaves is a located
// rejection with its own wording (authored here, binding) — int-leaf
// structs stay on the typed path and never expose their object
// representation; (2) a RUNTIME read of a flexible-array-member tail is
// a located rejection (the FAM declaration and its folded static
// initializer are legal, see byte-region-aggregates.c); (3) an unnamed
// union arm with non-u8 leaves whose size differs from the union's is
// not representable as an equal-size alias. FR-167 PHASE 2 MOVED THAT
// THIRD PIN FORWARD WITHOUT LOOSENING IT: the union DECLARATION now
// imports as FR-78's sizeof-sized opaque blob, so the refusal moves from
// the union's `{` to the STATIC INITIALIZER that names an arm --
// `unsupported: opaque union arm initializer`, at the initializer, which
// is FR-78's standing constraint that a blob admits only the all-zero
// constant. Still one located rejection per TU, at a strictly more
// precise site; the wording is pinned EXACTLY so a regression that
// silently accepted an arm initializer (a miscompile: the blob would
// carry zeros where C wrote 7) fails here.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/int-leaf-view.c 2>&1 | FileCheck %s --check-prefix=INTLEAF
// RUN: not emitrust-import-c %t/fam-tail-read.c 2>&1 | FileCheck %s --check-prefix=FAMREAD
// RUN: not emitrust-import-c %t/mixed-size-arm.c 2>&1 | FileCheck %s --check-prefix=MIXED

//--- int-leaf-view.c
// struct P has int leaves (with padding-free layout, even): its object
// representation is still not observable — the byte view rejects at the
// cast.
typedef unsigned char u8;
struct P { int x; int y; };
struct P gp = {1, 2};
int printf(const char *, ...);

void print_(const u8 *p, long size) {
  while (size--) {
    printf(" %x", *p++);
  }
}

int main(void) {
  print_((u8 *)&gp, sizeof gp);
  return 0;
}
// INTLEAF: int-leaf-view.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: byte view of an aggregate with non-byte members

//--- fam-tail-read.c
// The FAM tail of a u8-only region struct is initializable (extended
// image) but has no runtime-readable storage behind sizeof: any access
// to the tail member rejects at the access site.
typedef unsigned char u8;
struct V { u8 a, b; };
struct W {
  struct V v;
  u8 tail[];
};
struct W gw = {{1, 2}, {3, 4}};

int main(void) {
  return gw.tail[0];
}
// FAMREAD: fam-tail-read.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member access

//--- mixed-size-arm.c
// The unnamed arm's leaves are u16 (non-u8) and its 2-byte size differs
// from the union's 4 bytes: neither a u8-only view nor an equal-size
// alias, so the one-slot trial fails and phase 2's blob takes the
// declaration. The `= {{7}}` initializer names an arm the blob cannot
// represent, and THAT is what rejects now.
typedef unsigned char u8;
union Bad {
  struct { unsigned short w; };
  u8 raw[4];
};
union Bad gb = {{7}};

int main(void) {
  return gb.raw[0];
}
// MIXED: mixed-size-arm.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: opaque union arm initializer
// MIXED-NOT: error: unsupported: union
