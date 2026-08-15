// FR-77: taking the address of a function NO translation unit defines is a
// LOCATED import rejection at the address-taking site. The invariant this
// pins: a fn-ptr constant is emitted as the OPAQUE text `Some(<name>)`, which
// is NOT a SymbolUse, so a function whose only reference is a function-pointer
// initializer used to slip past finalizeProject's referenced-but-not-defined
// walk (the erase-unused branch saw an empty use list and erased the
// prototype) and the crate shipped a dangling `Some(<name>)` -- rustc E0425,
// the whole crate lost (tinycrypt ecc.c, the corpus's first CRATE_NOBUILD).
// Rejection is the feature: the diagnostic lands on the C construct, in every
// fn-ptr constant position (scalar initializer, aggregate table leaf), and
// identically under --externals-trait (a generic trait item is not a function
// item, so it has no value-level address the `Some(<name>)` could name).
// A definition in ANY sibling TU must keep resolving -- that is the
// cross-TU acceptance half of the pin.
//
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/noother.c %t/empty.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOOTHER
// RUN: not emitrust-import-c --externals-trait %t/noother.c %t/empty.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOOTHER
// RUN: not emitrust-import-c %t/table.c %t/empty.c 2>&1 \
// RUN:   | FileCheck %s --check-prefix=TABLE
// RUN: emitrust-import-c %t/noother.c %t/def.c \
// RUN:   | FileCheck %s --check-prefix=DEFINED

//--- empty.c
int unrelated(int v) { return v; }

//--- noother.c
// The initializer is the ONLY reference: the setter keeps fnptr-devirt from
// turning the call into a direct `call @helper` (which WOULD be a SymbolUse
// and was already rejected), so this is exactly the erase-unused escape.
extern int helper(int);
static int (*cb)(int) = &helper;
void set_cb(int (*f)(int)) { cb = f; }
int use_cb(int x) { int (*f)(int) = cb; return f ? f(x) : 0; }
// NOOTHER: noother.c:5:{{[0-9]+}}: error: unsupported: taking the address of undefined function 'helper'

//--- table.c
// The aggregate position: a fn-ptr TABLE initializer's leaves are the same
// opaque `Some(<name>)` spelling (APValue aggregate walk), so an undefined
// name inside an array initializer rejects the same way.
extern int op_a(int);
extern int op_b(int);
static int (*const ops[2])(int) = { &op_a, &op_b };
int run(int i, int x) { int (*f)(int) = ops[i]; return f ? f(x) : 0; }
// TABLE: table.c:6:{{[0-9]+}}: error: unsupported: taking the address of undefined function 'op_a'

//--- def.c
int helper(int x) { return x + 1; }

// The cross-TU definition case keeps working: the sibling TU's definition
// resolves the address, the global keeps its `Some(helper)` constant, and
// the defined function is in the module.
// DEFINED-DAG: emitrust.global @{{[A-Za-z0-9_]+}} <#emitrust.opaque<"Some(helper)">>
// DEFINED-DAG: func.func @helper(
