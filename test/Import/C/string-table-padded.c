// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// FR-217: a `static const char *const t[N] = {"a", "bb", ...}` table has
// no pointer representation -- `ArrayType::isValidElementType` admits no
// reference element (hand-written IR with a
// `!emitrust.ref<!emitrust.slice<i8>>` element is refused at parse time by
// both emitrust-opt and emitrust-translate), and the pointer value model
// gives each pointer ONE base object where a table of N literals needs N.
// So it imports in the PADDED two-dimensional byte form the emitter
// already supports: `!emitrust.array<Nx!emitrust.array<Wxi8>>` with W the
// longest element plus its NUL, every shorter row NUL-filled to W.
//
// This file pins the SHAPE (which is where a wrong width, a missing
// terminator, or an unscaled row index would be visible in the IR); the
// runtime oracle is test/EndToEnd/string-table-padded.c, whose stdout is
// byte-diffed against the clang native. Both are needed: the emitted crate
// builds cleanly under any of those errors.

int printf(const char *, ...);
int puts(const char *);

// W is 4 here: "ccc" plus its NUL. The two shorter rows pad with zeros,
// and the global is immutable (the element type is `const char *const`, so
// no element can ever be reassigned).
// CHECK: emitrust.global const @names <[
// CHECK-SAME: [97 : i8, 0 : i8, 0 : i8, 0 : i8]
// CHECK-SAME: [98 : i8, 98 : i8, 0 : i8, 0 : i8]
// CHECK-SAME: [99 : i8, 99 : i8, 99 : i8, 0 : i8]
// CHECK-SAME: ]> : !emitrust.array<3x!emitrust.array<4xi8>>
static const char *const names[3] = {"a", "bb", "ccc"};

// Each table gets its OWN width: a shared W would show up as extra zeros.
// CHECK: emitrust.global const @units <[
// CHECK-SAME: [115 : i8, 0 : i8, 0 : i8]
// CHECK-SAME: [109 : i8, 115 : i8, 0 : i8]
// CHECK-SAME: ]> : !emitrust.array<2x!emitrust.array<3xi8>>
static const char *const units[2] = {"s", "ms"};

// An empty element is one NUL, not an absent row.
// CHECK: emitrust.global const @maybe <[
// CHECK-SAME: [0 : i8, 0 : i8]
// CHECK-SAME: [122 : i8, 0 : i8]
// CHECK-SAME: ]> : !emitrust.array<2x!emitrust.array<2xi8>>
static const char *const maybe[2] = {"", "z"};

// A tentative declaration ahead of the definition: the table registers
// under the CANONICAL declaration, so a use that is imported BEFORE the
// definition is seen still takes the padded lowering rather than falling
// back to the pointer rejection.
static const char *const names[3];

int early(int i) {
  // CHECK-LABEL: func.func @early
  // CHECK: emitrust.global_load @names : !emitrust.array<3x!emitrust.array<4xi8>>
  printf("%s", names[i]);
  return 0;
}

int use(int i) {
  // `names[i]` is a POINTER in C but an i8-array place after the lowering,
  // so the row subscript peels one array dimension and the `%s` argument
  // borrows the whole row (NUL-stopped by the __emitrust_cstr helper),
  // exactly as `const char t[3][4]` would.
  // CHECK-LABEL: func.func @use
  // CHECK: %[[ROW:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<3x!emitrust.array<4xi8>>>, i32) -> !emitrust.lvalue<!emitrust.array<4xi8>>
  // CHECK: emitrust.slice_of %[[ROW]]
  printf("%s", names[i]);
  // puts takes the same shape.
  // CHECK: emitrust.subscript %{{.*}} -> !emitrust.lvalue<!emitrust.array<4xi8>>
  // CHECK: emitrust.slice_of
  puts(names[i]);
  // The SECOND subscript is an ordinary array index into the row, not a
  // pointer walk: no cursor arithmetic, no div/rem.
  // CHECK: %[[R2:.*]] = emitrust.subscript %{{.*}}[%{{.*}}] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.array<3xi8>>>, i32) -> !emitrust.lvalue<!emitrust.array<3xi8>>
  // CHECK: emitrust.subscript %[[R2]][%{{.*}}] : (!emitrust.lvalue<!emitrust.array<3xi8>>, i32) -> !emitrust.lvalue<i8>
  return units[i][i] + maybe[0][0];
}

// The emitted Rust is the padded 2-D literal, and the rows are indexed
// then sliced -- a byte of movement here is a behavior change.
// RUST: static TU0_NAMES: {{\[\[}}i8; 4]; 3] = {{\[\[}}97, 0, 0, 0], [98, 98, 0, 0], [99, 99, 99, 0]];
// RUST: static TU0_UNITS: {{\[\[}}i8; 3]; 2] = {{\[\[}}115, 0, 0], [109, 115, 0]];
// RUST: static TU0_MAYBE: {{\[\[}}i8; 2]; 2] = {{\[\[}}0, 0], [122, 0]];
