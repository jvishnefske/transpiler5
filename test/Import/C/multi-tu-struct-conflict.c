// C99-6 x CTS-R4: two translation units defining the same FILE-SCOPE tag
// `T` with different shapes is a genuine cross-TU conflict and keeps its
// located diagnostic. (Block-scope shadowing of a tag is NOT this case —
// see structs-shadow.c — the name-keyed dedup applies to file-scope
// records only.)
// RUN: not emitrust-import-c %s %S/Inputs/multi-tu-struct-conflict-other.c 2>&1 | FileCheck %s

struct T {
  int x;
};

int main(void) {
  struct T t;
  t.x = 0;
  return t.x;
}

// CHECK: multi-tu-struct-conflict-other.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: conflicting definition of struct 'T' with a different shape in another translation unit
