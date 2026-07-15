// RUN: emitrust-import-c %s | FileCheck %s

// C99-14: file-scope variables become module-level emitrust.global
// definitions with constant-evaluated initializers; C99-15: function-local
// statics become mangled globals. Accesses go through
// emitrust.global_load/global_store.

int printf(const char *, ...);

// A tentative definition (and its repetition) is one zero-initialized
// global.
// CHECK: emitrust.global @counter : i32
int counter;
int counter;

// An extern declaration reconciles with the later definition.
// CHECK: emitrust.global @limit <100 : i32> : i32
extern int limit;
int limit = 100;

// A const-qualified scalar becomes an immutable global.
// CHECK: emitrust.global const @scale <3 : i32> : i32
const int scale = 3;

// static file-scope variables keep their name; float, bool, and char
// initializers are constant-evaluated.
// CHECK: emitrust.global @ratio <2.500000e+00 : f64> : f64
static double ratio = 2.5;
// CHECK: emitrust.global @flag <true> : i1
_Bool flag = 1;
// CHECK: emitrust.global @letter <97 : i8> : i8
char letter = 'a';

// Aggregate globals without initializers are zero-initialized (the type's
// default value).
// CHECK: emitrust.global @table : !emitrust.array<4xi32>
int table[4];
// CHECK: emitrust.struct_def @Point ["x", "y"] [i32, i32]
// CHECK: emitrust.global @origin : !emitrust.struct<"Point">
struct Point {
  int x;
  int y;
};
struct Point origin;

// Whole-value scalar accesses are direct global_load/global_store pairs.
// CHECK-LABEL: func.func @bump
// CHECK: %[[CUR:.*]] = emitrust.global_load @counter : i32
// CHECK: %[[NEXT:.*]] = arith.addi %[[CUR]], %{{.*}} : i32
// CHECK: emitrust.global_store %[[NEXT]], @counter : i32
// CHECK: emitrust.global_load @counter : i32
int bump(void) {
  counter += 1;
  return counter;
}

// A function-local static is a module-level global mangled
// <function>_<name>, initialized once at program start.
// CHECK-LABEL: func.func @tick
// CHECK: emitrust.global_load @tick_calls : i32
// CHECK: emitrust.global_store %{{.*}}, @tick_calls : i32
// CHECK: emitrust.global @tick_calls <0 : i32> : i32
int tick(void) {
  static int calls = 0;
  calls++;
  return calls;
}

// Element writes stage the whole array in a local copy, update the
// element, and store the copy back (exact single-threaded semantics).
// CHECK-LABEL: func.func @set_table
// CHECK: %[[COPY:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[VAL:.*]] = emitrust.global_load @table : !emitrust.array<4xi32>
// CHECK: emitrust.assign %[[COPY]] = %[[VAL]] : !emitrust.lvalue<!emitrust.array<4xi32>>
// CHECK: %[[ELEM:.*]] = emitrust.subscript %[[COPY]][%{{.*}}]
// CHECK: emitrust.assign %[[ELEM]] = %{{.*}} : !emitrust.lvalue<i32>
// CHECK: %[[FULL:.*]] = emitrust.load %[[COPY]]
// CHECK: emitrust.global_store %[[FULL]], @table : !emitrust.array<4xi32>
void set_table(int i, int v) {
  table[i] = v;
}

// Element and field reads stage a copy but store nothing back.
// CHECK-LABEL: func.func @observe
// CHECK: emitrust.global_load @table : !emitrust.array<4xi32>
// CHECK: emitrust.subscript
// CHECK: emitrust.global_load @origin : !emitrust.struct<"Point">
// CHECK: emitrust.member %{{.*}}["x"]
// CHECK-NOT: emitrust.global_store
// CHECK: return
int observe(int i) {
  return table[i] + origin.x;
}

// Const and static-file-scope globals are read like any other.
// CHECK-LABEL: func.func @c_main
// CHECK: emitrust.global_load @scale : i32
// CHECK: emitrust.global_load @ratio : f64
// CHECK: emitrust.global_load @flag : i1
// CHECK: emitrust.global_load @letter : i8
int main(void) {
  counter = limit;
  set_table(1, scale);
  double r = ratio;
  if (flag) {
    counter += observe(1) + bump() + tick() + letter;
  }
  printf("%d %f\n", counter, r);
  return 0;
}
