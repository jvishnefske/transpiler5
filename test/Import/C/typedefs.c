// C99-6: typedefs of every supported type shape resolve through their
// canonical types: scalars (including chained typedefs), pointers used as
// reference parameters, arrays, tagged structs, and enums. The typedef is
// transparent -- the imported IR is identical to the spelled-out type.
// (Typedefs naming an anonymous struct are covered in
// structs-anon-typedef.c; function-pointer typedefs in fn-pointers.c.)
// RUN: emitrust-import-c %s | FileCheck %s

typedef unsigned int u32;
typedef u32 word; // chained typedef
typedef int *int_ref;
typedef int quad[4];
struct Pair {
  int a;
  int b;
};
typedef struct Pair Pair;
enum Mode { Off, On };
typedef enum Mode Mode;

word add_words(u32 a, word b) { return a + b; }

void bump(int_ref p) { *p = *p + 1; }

int sum_quad(quad q) { return q[0] + q[1] + q[2] + q[3]; }

int pair_sum(Pair p) { return p.a + p.b; }

int is_on(Mode m) { return m == On; }

// The typedef'd tagged struct and enum keep their tag names.
// CHECK-DAG: emitrust.struct_def @Pair ["a", "b"] [i32, i32]
// CHECK-DAG: emitrust.enum_def @Mode ["Off", "On"] [0, 1]

// Scalar typedefs, chained through a second typedef, are the underlying
// unsigned type.
// CHECK-LABEL: func.func @add_words
// CHECK-SAME: (%{{.*}}: ui32, %{{.*}}: ui32) -> ui32

// A pointer typedef parameter is the same mutable reference as `int *`.
// CHECK-LABEL: func.func @bump
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<i32>)

// An array typedef parameter decays exactly like `int q[4]`.
// CHECK-LABEL: func.func @sum_quad
// CHECK-SAME: (%{{.*}}: !emitrust.mut_ref<!emitrust.slice<i32>>) -> i32

// A typedef of a tagged struct is the tag-named struct type.
// CHECK-LABEL: func.func @pair_sum
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"Pair">) -> i32

// A typedef of a named enum is the named enum type.
// CHECK-LABEL: func.func @is_on
// CHECK-SAME: (%{{.*}}: !emitrust.enum<"Mode">) -> i32
