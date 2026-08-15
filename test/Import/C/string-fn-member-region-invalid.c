// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/u32-count.c 2>&1 | FileCheck %s --check-prefix=U32COUNT
// RUN: not emitrust-import-c %t/u32-odd.c 2>&1 | FileCheck %s --check-prefix=U32ODD
// RUN: not emitrust-import-c %t/u32-fill.c 2>&1 | FileCheck %s --check-prefix=U32FILL
// RUN: not emitrust-import-c %t/u32-memcpy.c 2>&1 | FileCheck %s --check-prefix=U32MEMCPY
// RUN: not emitrust-import-c %t/u32-memcmp.c 2>&1 | FileCheck %s --check-prefix=U32MEMCMP
// RUN: not emitrust-import-c %t/impure-index.c 2>&1 | FileCheck %s --check-prefix=IMPURE
// RUN: not emitrust-import-c %t/byte-record.c 2>&1 | FileCheck %s --check-prefix=BYTEREC
// RUN: not emitrust-import-c %t/mixed-member.c 2>&1 | FileCheck %s --check-prefix=MIXEDMEM
// RUN: not emitrust-import-c %t/strcpy-member.c 2>&1 | FileCheck %s --check-prefix=STRCPYMEM

// FR-87 frontier: member-array REGIONS for the hosted byte family cover
// EXACTLY the shapes the helpers can render as byte-exact safe Rust;
// everything else keeps a LOCATED rejection — rejection is a feature,
// and none of these shapes may silently emit code that is wrong or
// fails only downstream. The u32 subset is memset-DESTINATION-only and
// demands compile-time constants: a non-constant (or non-multiple-of-4)
// byte count cannot prove whole-word coverage, a non-constant fill byte
// has no compile-time replicated word (`b * 0x01010101`), and u32
// memcpy/memcmp have no admitted image at all. An IMPURE offset index
// (a call) declines the interception — the single borrow-point
// evaluation would reorder its side effects — and a BYTE-REGION record
// (every scalar leaf `unsigned char`) keeps the FR-83 blob model, whose
// members are windows of one region base, not places; both keep the
// historical decay rejection verbatim. FR-72's element-agreement rule
// composes unchanged: a mixed i8/ui8 member pair has no helper
// signature that fits both. The str*-family stays UNintercepted:
// FR-87 is byte-family-only, so a member char array as a strcpy
// destination keeps the historical rejection. Every wording below is
// pinned verbatim as measured against the built tool.

// A runtime byte count over a u32 member region: word coverage is not
// provable, so the call rejects at import.
// U32COUNT: u32-count.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over an unsigned int region requires a constant byte count that is a multiple of 4

//--- u32-count.c
#include <string.h>
struct K { unsigned int words[8]; };
struct C { struct K key; unsigned int n; };
void f(struct C *c, unsigned int n) {
  memset(c->key.words, 0x00, n);
}
int main(void) {
  struct C c;
  f(&c, 8u);
  return (int)(c.key.words[0] & 1u);
}

// A constant count that is NOT a multiple of 4 would split a word:
// no whole-word fill image covers it.
// U32ODD: u32-odd.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over an unsigned int region requires a constant byte count that is a multiple of 4

//--- u32-odd.c
#include <string.h>
struct K { unsigned int words[8]; };
struct C { struct K key; unsigned int n; };
void f(struct C *c) {
  memset(c->key.words, 0x00, 6);
}
int main(void) {
  struct C c;
  f(&c);
  return (int)(c.key.words[0] & 1u);
}

// A runtime fill byte has no compile-time replicated word.
// U32FILL: u32-fill.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memset over an unsigned int region requires a constant fill byte

//--- u32-fill.c
#include <string.h>
struct K { unsigned int words[8]; };
struct C { struct K key; unsigned int n; };
void f(struct C *c, int b) {
  memset(c->key.words, b, 8);
}
int main(void) {
  struct C c;
  f(&c, 3);
  return (int)(c.key.words[0] & 1u);
}

// u32 member regions are memset-destination-only: memcpy has no
// admitted word image.
// U32MEMCPY: u32-memcpy.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over an unsigned int region

//--- u32-memcpy.c
#include <string.h>
struct C { unsigned int a[8]; unsigned int b[8]; unsigned int n; };
void f(struct C *c) {
  memcpy(c->a, c->b, 32);
}
int main(void) {
  struct C c;
  c.b[0] = 5u;
  f(&c);
  return (int)(c.a[0] & 1u);
}

// ... and neither has memcmp (value position).
// U32MEMCMP: u32-memcmp.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: string function argument over an unsigned int region

//--- u32-memcmp.c
#include <string.h>
struct C { unsigned int a[8]; unsigned int b[8]; unsigned int n; };
int f(struct C *c) {
  return memcmp(c->a, c->b, 32);
}
int main(void) {
  struct C c;
  c.a[0] = 1u;
  c.b[0] = 2u;
  return f(&c) & 1;
}

// FR-86 purity gate, region position: a function-CALL index cannot be
// evaluated exactly once at the borrow point without reordering its
// side effects, so the interception declines and the historical decay
// rejection fires unchanged.
// IMPURE: impure-index.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- impure-index.c
#include <string.h>
struct C { unsigned char V[16]; unsigned int n; };
static unsigned int g(void) { static unsigned int i; return i++; }
void f(struct C *c) {
  memset(&c->V[g()], 0, 4);
}
int main(void) {
  struct C c;
  f(&c);
  return (int)(c.V[0] & 1u);
}

// A byte-region record (every scalar leaf `unsigned char`): its members
// are windows of the FR-83 blob region, not places — the member
// interception declines and the historical rejection stays.
// BYTEREC: byte-record.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- byte-record.c
#include <string.h>
struct B { unsigned char x[8]; unsigned char y[8]; };
void f(struct B *b) {
  memset(b->x, 0, 8);
}
int main(void) {
  struct B b;
  f(&b);
  return (int)(b.x[0] & 1u);
}

// FR-72's element-agreement rule composes with member regions: a MIXED
// call (ui8 member destination, i8 char member source — disjoint
// sibling fields, so the aliasing guard admits) still has no helper
// signature both regions satisfy, and rejects at import instead of
// E0308 in the emitted crate.
// MIXEDMEM: mixed-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: memcpy arguments mix char and unsigned char regions

//--- mixed-member.c
#include <string.h>
struct C { unsigned char b[8]; char name[8]; unsigned int n; };
void f(struct C *c) {
  memcpy(c->b, c->name, 8);
}
int main(void) {
  struct C c;
  c.name[0] = 'x';
  f(&c);
  return (int)(c.b[0] & 1u);
}

// Byte-family-only scope: the str*-family keeps its historical
// frontier — a member char array as a strcpy destination still rejects.
// STRCPYMEM: strcpy-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported pointer cast (ArrayToPointerDecay)

//--- strcpy-member.c
#include <string.h>
struct T { char name[8]; unsigned int n; };
void f(struct T *t) {
  strcpy(t->name, "hi");
}
int main(void) {
  struct T t;
  f(&t);
  return (int)t.name[0] & 1;
}
