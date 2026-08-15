// FR-77, link-shard flavor of the fn-ptr-target obligation: a shard whose
// ONLY reference to an external function is a fn-ptr constant's opaque
// `Some(<name>)` must still hand the FR-58 link step the obligation. The
// finalize walk's erase-unused branch ran BEFORE the defer branch and a
// `Some(<name>)` is not a SymbolUse, so the shard used to erase the target's
// prototype, the link step never learned the name, and a link line missing
// the definition merged silently into a crate with a dangling `Some(helper)`.
// This pins the fix: in defer mode the address-taken prototype is MARKED
// (`emitrust.extern_decl`), so an unresolved target is the located
// undefined-symbol link error, and a link line that does define it keeps
// resolving to the ordinary fn-ptr constant.
//
// Undefined at link: nothing else on the link line defines `helper`.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: not emitrust-cc --link %t.main.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=UNRESOLVED
// UNRESOLVED: link-merge-fnptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unresolved external 'helper' at link
//
// Defined by a companion shard: the merge resolves the obligation and the
// emitted Rust carries both the constant and the definition.
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c \
// RUN:   %S/Inputs/link-merge-fnptr-def.c -o %t.def.o
// RUN: emitrust-cc --link %t.main.o %t.def.o --emit=rust -o %t.ok.rs
// RUN: FileCheck %s --check-prefix=LINKED < %t.ok.rs
// LINKED-DAG: Some(helper)
// LINKED-DAG: fn helper(

extern int helper(int);
static int (*cb)(int) = &helper;
void set_cb(int (*f)(int)) { cb = f; }
int use_cb(int x) {
  int (*f)(int) = cb;
  return f ? f(x) : 0;
}
int main(void) { return use_cb(2) == 6 ? 0 : 1; }
