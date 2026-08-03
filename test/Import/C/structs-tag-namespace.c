// RUN: emitrust-import-c %s | FileCheck %s

// CTS-R5: C keeps struct tags and ordinary identifiers in separate
// namespaces (C99 6.2.3), but the module has a single symbol table. A tag
// that collides with an ordinary identifier (a global, a function, or the
// <function>_<name> mangle of a function-local static) is deterministically
// renamed Struct_<tag>; a collision-free tag keeps its readable spelling.
// The rename is declaration-order independent: `a` is declared before its
// struct, `f` after its struct.

int a = 1;

struct a {
  int value;
};

struct b {
  int value;
};

struct f {
  int value;
};

int f(void) { return 2; }

int tick(void) {
  static int calls;
  calls = calls + 1;
  return calls;
}

struct tick_calls {
  int value;
};

int main(void) {
  struct a sa;
  sa.value = a;
  struct b sb;
  sb.value = f();
  struct f sf;
  sf.value = tick();
  struct tick_calls st;
  st.value = 4;
  return sa.value + sb.value + sf.value + st.value - 8;
}

// The colliding tags yield to the ordinary namespace under the Struct_
// prefix; struct b keeps its bare name because nothing ordinary claims it.
// CHECK-DAG: emitrust.global @a <1 : i32> : i32
// CHECK-DAG: emitrust.struct_def @Struct_a ["value"] [i32]
// CHECK-DAG: emitrust.struct_def @b ["value"] [i32]
// CHECK-DAG: emitrust.struct_def @Struct_f ["value"] [i32]
// CHECK-DAG: emitrust.global @tick_calls
// CHECK-DAG: emitrust.struct_def @Struct_tick_calls ["value"] [i32]

// Every mention of the renamed types goes through the assigned name.
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.variable named "sa" : !emitrust.lvalue<!emitrust.struct<"Struct_a">>
// CHECK: emitrust.variable named "sb" : !emitrust.lvalue<!emitrust.struct<"b">>
// CHECK: emitrust.variable named "sf" : !emitrust.lvalue<!emitrust.struct<"Struct_f">>
// CHECK: emitrust.variable named "st" : !emitrust.lvalue<!emitrust.struct<"Struct_tick_calls">>
