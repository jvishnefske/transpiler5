// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// C99-15: a function-local static is mangled <function>_<name>; a collision
// with an existing module symbol is rejected, never silently merged.
int tick_calls;

int tick(void) {
  static int calls = 0;
  calls++;
  return calls;
}

int main(void) {
  return tick();
}

// CHECK: globals-static-collision.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global variable 'tick_calls' collides with an existing symbol
