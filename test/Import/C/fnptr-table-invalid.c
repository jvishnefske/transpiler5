// CTS-BR (00216) negative space: the function-pointer table admission
// covers only tables whose slots are NEVER reassigned after their
// initializer — a runtime store into a table slot is a located
// rejection with its own wording (authored here, binding). This keeps
// the folded Some(target) element list a static fact.
//
// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

int printf(const char *, ...);

void first(void) { printf("first\n"); }
void second(void) { printf("second\n"); }

typedef void (*fptr)(void);
fptr table[2] = { first, second };

void swap_in(void) {
  table[0] = second;
}

int main(void) {
  swap_in();
  table[0]();
  return 0;
}
// CHECK: fnptr-table-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: assignment to a function-pointer array element
