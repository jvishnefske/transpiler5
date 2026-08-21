// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/varargs.c 2>&1 | FileCheck %s --check-prefix=VARARGS
// RUN: not emitrust-import-c %t/struct.c 2>&1 | FileCheck %s --check-prefix=STRUCT
// RUN: not emitrust-import-c %t/fnptr.c 2>&1 | FileCheck %s --check-prefix=FNPTR
// RUN: not emitrust-import-c %t/decl-only.c 2>&1 | FileCheck %s --check-prefix=DECLONLY
// RUN: not emitrust-import-c %t/constptr.c 2>&1 | FileCheck %s --check-prefix=CONSTPTR
// RUN: not emitrust-import-c %t/alias.c 2>&1 | FileCheck %s --check-prefix=ALIAS
// RUN: not emitrust-import-c %t/addr-taken.c 2>&1 | FileCheck %s --check-prefix=ADDRTAKEN
// RUN: not emitrust-import-c %t/glob.c 2>&1 | FileCheck %s --check-prefix=GLOB

// FR-100 frontier: the callee-aware scalar-forwarding exception
// (scalar-out-param-forward.c) admits EXACTLY a bare reference to an
// arithmetic-pointee pointer parameter, in a direct-call argument
// position, to a non-variadic in-TU DEFINITION of matching arity whose
// own corresponding parameter is itself a scalar reference. Everything
// outside that envelope keeps today's conservative Slice class and
// therefore its historical LOCATED rejection: rejection is a feature,
// and none of these shapes may silently borrow one element of a region
// the callee may walk. Every wording below is pinned verbatim as
// measured against the built tool.

// A VARIADIC callee has no positional parameter to key the class on
// past its named ones, so no forwarding edge is recorded and the
// caller's parameter stays a slice.
// VARARGS: varargs.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- varargs.c
static void v(int *p, ...) { *p += 1; }
static void f(int *p) { v(p, 1); }
int main(void) { int n = 1; f(&n); return n; }

// NON-ARITHMETIC pointee (a struct out-parameter, the most common C
// out-param shape after scalars): the FR-100 rule is keyed on
// arithmetic pointees only, so no edge is recorded.
// STRUCT: struct.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- struct.c
struct S { int a; int b; };
static void t(struct S *s) { s->a = 1; }
static void f(struct S *s) { t(s); }
int main(void) { struct S s; f(&s); return s.a; }

// INDIRECT call through a function pointer: there is no direct callee,
// so the region contract behind the parameter is unknown.
// FNPTR: fnptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- fnptr.c
static void g(int *p) { *p += 2; }
static void f(int *p, void (*fp)(int *)) { fp(p); }
int main(void) { int n = 1; f(&n, g); return n; }

// A callee with NO in-TU definition (declared here, defined elsewhere):
// its region contract is not resolvable at classification time, so the
// forwarding parameter must stay conservative. The cross-TU form of the
// same rule is pinned by pointers-param-invalid.c's CROSSTU arm, whose
// refinement diagnostic keeps firing verbatim.
// DECLONLY: decl-only.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- decl-only.c
void ext(int *p);
static void f(int *p) { ext(p); }
int main(void) { int n = 1; f(&n); return n; }

// Not a BARE parameter reference: forwarding an `int *` into a
// `const int *` parameter inserts a qualification conversion, so the
// argument is not the block argument itself and the symbol-type
// equality that proves the callee agreed on the class cannot hold.
// CONSTPTR: constptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the address of a scalar object cannot be passed as a slice parameter

//--- constptr.c
static int rd(const int *a) { return *a; }
static int f(int *p) { return rd(p); }
int main(void) { int n = 9; return f(&n); }

// ALIASING is unaffected: forwarding the SAME parameter into two
// mutable parameters of one call would be two simultaneous &mut
// borrows of one object, and keeps its located rejection instead of
// emitting Rust that cannot compile.
// ALIAS: alias.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: aliasing mutable pointer arguments (two arguments borrow object 'p')

//--- alias.c
static void g(int *a, int *b) { *a += *b; }
static void f(int *p) { g(p, p); }
int main(void) { int n = 5; f(&n); return n; }

// A post-fixpoint OVERRIDE still wins: FR-76 forces the address-taken
// callee's parameter to Slice AFTER the forwarding fixpoint ran, so the
// caller's assumption no longer holds — and the disagreement surfaces
// as a located rejection at the forwarding site, never as wrong code.
// ADDRTAKEN: addr-taken.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer parameter used outside a direct dereference

//--- addr-taken.c
static void g(int *p) { *p += 1; }
static void f(int *p) { g(p); }
static void (*tbl)(int *) = g;
int main(void) { int n = 0; f(&n); tbl(&n); return n; }

// The address of a GLOBAL forwarded through a scalar chain: the global
// keeps its own located rejection (a global is not a borrowable local
// place), unchanged in kind by FR-100.
// GLOB: glob.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of a global variable

//--- glob.c
static int gv = 3;
static void g(int *p) { *p += 1; }
static void f(int *p) { g(p); }
int main(void) { f(&gv); return gv; }
