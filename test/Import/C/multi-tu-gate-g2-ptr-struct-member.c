// W3.1 multi-TU gate oracle (G2) — predicted failure #3: a struct member
// binding (CTS-P2) resolves through `memberPtrBindings`, a map built by
// merging every function body's bindings WITHIN ONE TU (`planOwners`
// Pass A) plus the constant-initializer walk of global struct objects.
// When the struct INSTANCE is externally visible, another TU could rebind
// the pointer member behind this TU's already-consumed facts, so
// `resolveMemberPointerFacts` (ImportC.cpp:1684) hard-rejects both the
// read and the write side unconditionally, regardless of whether the
// other TU's binding would actually agree.
//
// W3.2 will flip the `not` RUN line below to `emitrust-import-c ... |
// FileCheck` once member-pointer facts are merged across every AST in the
// project the way `globalPtrFacts` for CTS-P4 already intends to be
// (see planOwners's doc comment on `memberPtrBindings`).
//
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-gate-g2-ptr-struct-member-other.c 2>&1 | FileCheck %s

struct Node {
  int val;
  struct Node *next;
};

// `head` has external linkage (not `static`): the companion TU binds and
// reads its `next` member too.
struct Node head;
struct Node tail;

void link_nodes(void) { head.next = &tail; }

// CHECK: multi-tu-gate-g2-ptr-struct-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer struct member of an externally visible global in a multi-file project
