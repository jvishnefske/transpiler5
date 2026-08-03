// RUN: emitrust-import-c %s | FileCheck %s

// C99-13: compound literals in expression position. A block-scope compound
// literal of struct or array type materializes as a fresh anonymous
// `emitrust.variable` (default-initialized, then the C99-11 per-element
// assigns), and from there behaves as an ordinary lvalue of that temp:
// whole-value loads for value uses, member/subscript places for element
// uses, and a pointer-region base for its decay or address-of.

struct S {
  int a;
  int b;
};

// An initializer copy (`struct S s = (struct S){...}`) initializes the
// variable directly through the literal's list — no temp, no copy.
// CHECK-LABEL: func.func @init_copy
// CHECK: %[[S:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[A:.*]] = emitrust.member %[[S]]["a"]
// CHECK: emitrust.assign %[[A]]
// CHECK: %[[B:.*]] = emitrust.member %[[S]]["b"]
// CHECK: emitrust.assign %[[B]]
// CHECK-NOT: emitrust.load {{.*}}!emitrust.struct<"S">
int init_copy(void) {
  struct S s = (struct S){1, 2};
  return s.a + s.b;
}

// An assignment builds the temp first and copies it whole, so a
// self-referencing literal (`s = (struct S){s.b, s.a}`) reads the old
// values before the store — the lost-copy-safe swap shape.
// CHECK-LABEL: func.func @assign_swap
// CHECK: %[[DST:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[TMP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: emitrust.member %[[TMP]]["a"]
// CHECK: emitrust.member %[[TMP]]["b"]
// CHECK: %[[V:.*]] = emitrust.load %[[TMP]] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.struct<"S">
// CHECK: emitrust.assign %[[DST]] = %[[V]]
int assign_swap(void) {
  struct S s = {1, 2};
  s = (struct S){s.b, s.a};
  return s.a * 10 + s.b;
}

// Member access and subscript directly on the literal resolve on the
// temp's place like on a named local.
// CHECK-LABEL: func.func @member_use
// CHECK: %[[T:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[MB:.*]] = emitrust.member %[[T]]["b"]
// CHECK: emitrust.assign %[[MB]]
// CHECK: %[[RB:.*]] = emitrust.member %[[T]]["b"]
// CHECK: emitrust.load %[[RB]]
int member_use(void) { return (struct S){5, 6}.b; }

// CHECK-LABEL: func.func @subscript_use
// CHECK: %[[ARR:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
// CHECK: emitrust.subscript %[[ARR]]
int subscript_use(int i) { return (int[]){7, 8, 9}[i]; }

// A decayed array literal is a pointer-region base (cursor 0). The temp
// is hoisted to the entry block (region dereferences must be dominated),
// and each evaluation first restores the pristine default captured right
// after the creation — C's fresh zero-filled object per evaluation.
// CHECK-LABEL: func.func @ptr_array
// CHECK: %[[P:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
// CHECK-NEXT: %[[ZERO:.*]] = emitrust.load %[[P]]
// CHECK: emitrust.assign %[[P]] = %[[ZERO]]
// CHECK: emitrust.subscript %[[P]]
int ptr_array(void) {
  int *p = (int[]){10, 20, 30};
  p++;
  return *p + p[1];
}

// The address of a struct literal is a degenerate (cursor-less) region
// base: `q->a` resolves to a member place on the temp.
// CHECK-LABEL: func.func @ptr_struct
// CHECK: %[[Q:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: emitrust.member %[[Q]]["a"]
int ptr_struct(void) {
  struct S *q = &(struct S){40, 50};
  return q->a + q->b;
}

// A by-value struct argument loads the temp whole at the call site.
// CHECK-LABEL: func.func @arg_value
// CHECK: %[[AT:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[AV:.*]] = emitrust.load %[[AT]] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.struct<"S">
// CHECK: call @sum(%[[AV]])
static int sum(struct S s) { return s.a + s.b; }
int arg_value(void) { return sum((struct S){1, 2}); }

// A decayed literal passed to a slice parameter reslices the temp from
// cursor 0, like a decayed named array.
// CHECK-LABEL: func.func @arg_slice
// CHECK: %[[SB:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<2xi32>>
// CHECK: emitrust.slice_of mut %[[SB]]
// CHECK: call @first_two
static int first_two(int *p) { return p[0] + p[1]; }
int arg_slice(void) { return first_two((int[]){5, 6}); }

// A returned struct literal loads the temp whole (clang wraps the full
// expression in ExprWithCleanups; the temp's end-of-life needs no code).
// CHECK-LABEL: func.func @ret_value
// CHECK: %[[RT:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[RV:.*]] = emitrust.load %[[RT]]
// CHECK: return %[[RV]]
struct S ret_value(void) { return (struct S){3, 4}; }

// A `(char[N]){"..."}` literal fills its temp like a string-initialized
// char array declaration (C99-28 policy).
// CHECK-LABEL: func.func @char_literal
// CHECK: %[[CS:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi8>>
// CHECK: emitrust.assign {{.*}} : !emitrust.lvalue<i8>
int char_literal(void) {
  char *s = (char[]){"hi"};
  return s[0] + s[1];
}

// A nested struct literal as an aggregate initializer element initializes
// the element place directly through its own list.
// CHECK-LABEL: func.func @nested_element
// CHECK: %[[NT:.*]] = emitrust.variable named "t" : !emitrust.lvalue<!emitrust.struct<"T">>
// CHECK: %[[IN:.*]] = emitrust.member %[[NT]]["inner"]
// CHECK: emitrust.member %[[IN]]["a"]
struct T {
  struct S inner;
  int c;
};
int nested_element(void) {
  struct T t = {(struct S){8, 9}, 10};
  return t.inner.b + t.c;
}
