// W3.1 multi-TU gate oracle (G2 NEGATIVE): unlike
// multi-tu-gate-g2-ptr-struct-member.c (a single consistent binding the
// gate over-rejects today), this file's two translation units bind
// `head.next` to TWO DIFFERENT objects (`t1` here, `t2` in the
// companion) — a genuinely divergent cross-TU aliasing of the same
// externally visible instance's pointer member. Even after W3.2 merges
// member-pointer facts across the whole project, this must stay
// rejected: the per-instance model (one MemberPointerFacts entry per
// (instance, field)) has no representation for "the target depends on
// which TU last ran" (C itself makes this well-defined only because the
// LAST link-time store wins at runtime; the importer's static binding
// model has no execution-order-sensitive facts). This test only pins
// TODAY's blanket rejection, which happens to already be correct for
// this shape; W3.2 must not accidentally start accepting it.
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g2-ptr-struct-member-diverge-other.c 2>&1 | FileCheck %s

struct Node {
  int val;
  struct Node *next;
};

struct Node head;
struct Node t1;

void link1(void) { head.next = &t1; }

// CHECK: multi-tu-gate-g2-ptr-struct-member-diverge.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member of an externally visible global in a multi-file project
