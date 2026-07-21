// W3.1 multi-TU gate oracle (G2) — RETAINED (W3.4, design.md FR-35). A
// struct member binding (CTS-P2) resolves through `memberPtrBindings`, a
// map built by merging every function body's bindings WITHIN ONE TU
// (`planOwners` Pass A) plus the constant-initializer walk of global struct
// objects. When the struct INSTANCE is externally visible,
// `resolveMemberPointerBinding` (ImportC.cpp) hard-rejects in a multi-file
// project because another TU could rebind the pointer member behind this
// TU's already-consumed facts.
//
// Unlike G1/G8 (whose whole-program relaxations keep all emission TU-local),
// this shape reads `head.next->val` in the COMPANION TU, where the pointer
// member is statically devirtualized to a direct `emitrust.global_load @tail`
// — but `tail` is defined in the OTHER TU and this TU holds no `VarDecl` for
// it and never parsed the `head.next = &tail` binding. Accepting it needs
// the consuming TU to fabricate a by-symbol reference to a global it never
// declared, driven by a symbol-keyed member-binding fact it never saw: the
// cross-TU-emission problem. Disproportionate plumbing for a thin-demand
// shape whose sound rejection is already correct (the same "conservative-
// but-sound, disables a shape not correctness" framing as the G7 retention).
// RETAINED with the sound rejection pinned; the corpus (Track 4) is the
// signal that would reopen it.
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
