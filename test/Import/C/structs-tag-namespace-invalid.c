// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// CTS-R5 negative: a tag colliding with an ordinary identifier renames to
// Struct_<tag>; when the ordinary namespace claims that spelling too, the
// import is a located rejection, never a silent merge.

int a = 1;
int Struct_a = 2;

struct a {
  int value;
};

int main(void) {
  struct a x;
  x.value = a + Struct_a;
  return x.value - 3;
}

// CHECK: structs-tag-namespace-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct 'a' collides with an ordinary identifier, and so does its renamed spelling 'Struct_a'
