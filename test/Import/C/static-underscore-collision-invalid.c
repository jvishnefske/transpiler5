// FR-73 boundary: the underscore fold (`tu0_` + `_set` -> `tu0_set`) must
// not silently merge two C symbols. When a DIFFERENT raw spelling in the
// same TU composes to the same emitted name, the import rejects with a
// located diagnostic at the later declaration — never a silent bind. The
// decl-only case is the dangerous one: without the guard, a prototype-only
// `static int _set(int)` would be "satisfied" by `set`'s definition and
// every `_set(...)` call would silently execute set's body (a miscompile,
// not a build failure). Globals get the same guard: in defer-externals
// mode two tentative definitions of the same emitted name would otherwise
// quietly unify.
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/fn-def-def.c %t/other.c 2>&1 | FileCheck %s --check-prefix=DEFDEF
// RUN: not emitrust-import-c %t/fn-decl-def.c %t/other.c 2>&1 | FileCheck %s --check-prefix=DECLDEF
// RUN: not emitrust-import-c %t/globals.c %t/other.c 2>&1 | FileCheck %s --check-prefix=GLOBAL

//--- other.c
// Companion TU so the import is multi-file and per-TU tags are assigned (a
// single-TU emitrust-import-c run has an empty tag and no fold to guard).
int other_unused(int x) { return x; }

//--- fn-def-def.c
static int _set(int a) { return a + 1; }
static int set(int a) { return a + 2; }
int main(void) { return _set(1) + set(2); }
// DEFDEF: fn-def-def.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'set' emits as 'tu0_set', which collides with '_set' (leading underscores fold into the symbol prefix)

//--- fn-decl-def.c
static int _set(int);
static int set(int a) { return a + 2; }
int main(void) { return set(2) + _set(1); }
// DECLDEF: fn-decl-def.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'set' emits as 'tu0_set', which collides with '_set' (leading underscores fold into the symbol prefix)

//--- globals.c
static int _x;
static int x;
int main(void) { return _x + x; }
// GLOBAL: globals.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global variable 'x' emits as 'tu0_x', which collides with '_x' (leading underscores fold into the symbol prefix)
