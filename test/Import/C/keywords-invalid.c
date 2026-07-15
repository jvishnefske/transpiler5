// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/struct-name.c 2>&1 | FileCheck %s --check-prefix=STRUCT
// RUN: not emitrust-import-c %t/field-name.c 2>&1 | FileCheck %s --check-prefix=FIELD
// RUN: not emitrust-import-c %t/enum-name.c 2>&1 | FileCheck %s --check-prefix=ENUM
// RUN: not emitrust-import-c %t/fn-name.c 2>&1 | FileCheck %s --check-prefix=FN
// RUN: not emitrust-import-c %t/c-main.c 2>&1 | FileCheck %s --check-prefix=CMAIN

// C identifiers are emitted verbatim as Rust identifiers, so a spelling
// that Rust reserves is rejected wherever it would surface in the output:
// struct type names, struct member names, enum type names, and function
// names. `c_main` is additionally reserved because C `main` is renamed to
// it for the driver's Rust `main` wrapper. Parameter names never need the
// check: the Rust emitter synthesizes vN names for all SSA values. All
// diagnostics carry the file:line:col location.

// STRUCT: struct-name.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct name 'type' is a Rust keyword
// FIELD: field-name.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member 'fn' is a Rust keyword
// ENUM: enum-name.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: enum name 'match' is a Rust keyword
// FN: fn-name.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'loop' is a Rust keyword
// CMAIN: c-main.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: function name 'c_main' is reserved for the imported C main

//--- struct-name.c
struct type {
  int a;
};

int use_it(struct type t) { return t.a; }

//--- field-name.c
struct point {
  int fn;
};

int get(struct point p) { return p.fn; }

//--- enum-name.c
enum match { A, B };

int pick(enum match m) { return m == A; }

//--- fn-name.c
int loop(int x) { return x + 1; }

//--- c-main.c
int c_main(void) { return 1; }

int main(void) { return c_main(); }
