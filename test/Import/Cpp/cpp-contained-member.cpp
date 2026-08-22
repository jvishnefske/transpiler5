// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not="@Vec2_(" \
// RUN:   --implicit-check-not="@UOp_("
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST \
// RUN:   --implicit-check-not="fn vec2_(" --implicit-check-not="fn Vec2_(" \
// RUN:   --implicit-check-not="fn u_op_("

// FR-112 positive pin: an OVERLOADED OPERATOR is OMITTED from the class's
// imported methods -- on the struct path AND on the union path -- and
// omitting it keeps the rest of the class importable. This extends FR-117's
// non-identifier omission (conversion functions, pinned in
// cpp-conversion-function.cpp) to the operator half of the same
// `DeclarationName` family, replacing the W2.2 class-level
// `unsupported: overloaded operator` gate in `collectRecordFields` and its
// union-path twin in `importRecordUncached`.
//
// Omission (rather than rejecting the whole class) is sound because every
// USE of a member operator is a LOCATED rejection at the call: the spelled
// `a + b` / `b = a` forms and the implicit enclosing-`operator=` channel all
// reach the non-identifier-callee guard in the call dispatch
// (`unsupported: call to overloaded operator ... omitted from class ...`),
// and the explicit `a.operator+(b)` member-call spelling stays
// `unsupported: overloaded operator`. All pinned in
// cpp-contained-member-invalid.cpp.
//
// The union section is the half design.md's FR-112 entry left open: the
// union path never runs `collectRecordFields`, so FR-117 had given it its
// OWN class-level operator gate to keep the importer and FR-41's coloring
// screen in agreement. Removing only the struct gate would have re-minted
// the union FALSE RED (screen green, importer red); both gates go together,
// and the coloring screen goes with them
// (test/Project/coloring-cpp-class-gates.cpp).
//
// Sibling methods keep their exact names: the overload-count loop in
// `cxxMethodMangledName` skips empty-base-name members (FR-117), so an
// omitted operator cannot perturb the suffixing -- which is what keeps this
// change out of every `--emit=crate` golden byte.

struct Vec2 {
  int x;
  int y;

  // Both siblings import under their ordinary names.
  void set(int a, int b) { x = a; y = b; }
  int sum() const { return x + y; }

  // Omitted: no identifier spelling to mangle, every use located-rejected.
  int operator+(const Vec2 &o) const { return x + o.x; }
  bool operator==(const Vec2 &o) const { return x == o.x && y == o.y; }
};

union UOp {
  int a;

  // The union path's named methods import; its operator is omitted exactly
  // like the struct path's.
  int get() const { return a; }
  int operator+(int n) const { return a + n; }
};

int use(int n) {
  Vec2 v;
  v.set(n, n + 1);
  UOp u;
  u.a = v.sum();
  return u.get();
}

// CHECK: emitrust.struct_def @Vec2 ["x", "y"] [i32, i32]
// CHECK: func.func @Vec2_set(
// CHECK: func.func @Vec2_sum(
// CHECK: emitrust.struct_def @UOp
// CHECK: func.func @UOp_get(
// CHECK: func.func @use_(

// RUST: impl Vec2 {
// RUST: pub fn vec2_set(&mut self,
// RUST: pub fn vec2_sum(&self) -> i32 {
// RUST: impl UOp {
// RUST: pub fn u_op_get(&self) -> i32 {
