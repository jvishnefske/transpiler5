// FR-110: the emitted-Rust half of the method-name strip, end of pipe
// through emitrust-cc's idiomatic rename. Pins that every genuine C++
// method prints its in-impl spelling -- def AND call site -- while the
// mangled module symbols (`box_i32_get`, `noisy_drop`, ...) never reach
// the Rust text:
//  * instance methods and the receiver-taking constructor: `fn new`,
//    `fn get`, `fn set`, called as `bi.new(..)`/`bi.get()` (the
//    `BoxI32::new(x)` associated-fn form needs receiver rewriting and is
//    a separate increment);
//  * a static method through the qualified call_opaque form:
//    `BoxI32::origin()`, not `BoxI32::box_i32_origin()`;
//  * a W2.25 operator member: `fn op_eq` / `bi.op_eq(&b2)`;
//  * the collision-shaped droppy class: user methods literally named
//    `drop` and `clone` strip to INHERENT `fn drop`/`fn clone` beside
//    `impl Drop for Noisy` and its trait `fn drop` -- legal Rust, correct
//    binding (inherent wins at call sites, RAII still runs Drop::drop;
//    proven byte-identical in the FR-110 spike and re-proven by
//    test/EndToEnd/cpp-method-names.cpp), never a silent misbinding.
// RUN: emitrust-cc --emit=rust %s -o - | FileCheck %s

extern "C" int printf(const char *, ...);

class BoxI32 {
public:
  BoxI32(int v) : value(v) {}
  int get() const { return value; }
  void set(int v) { value = v; }
  static int origin() { return 0; }
  bool operator==(const BoxI32 &o) const { return value == o.value; }

private:
  int value;
};

struct Noisy {
  int id;
  Noisy(int i) : id(i) { printf("ctor %d\n", id); }
  void drop() { printf("user drop %d\n", id); }
  int clone() const { return id; }
  ~Noisy() { printf("bye %d\n", id); }
};

int main(int argc, char **argv) {
  BoxI32 bi(argc);
  bi.set(argc + 2);
  int v = bi.get();
  BoxI32 b2(argc + 2);
  int e = (bi == b2) ? 1 : 0;
  int o = BoxI32::origin();
  Noisy n(argc);
  n.drop();
  int c = n.clone();
  printf("%d %d %d %d\n", v, e, o, c);
  return 0;
}

// The driver body: stripped call sites on the mangled-symbol IR.
// CHECK: let mut bi: BoxI32 =
// CHECK: bi.new(
// CHECK: bi.set(
// CHECK: bi.get()
// CHECK: b2.new(
// CHECK: bi.op_eq(
// CHECK: BoxI32::origin()
// CHECK-NOT: box_i32_
// CHECK: n.new(
// CHECK: n.drop();
// CHECK: n.clone()
// CHECK-NOT: noisy_

// The impls: stripped member defs (appended at the end of the module).
// CHECK: impl BoxI32 {
// CHECK: fn new(&mut self, v: i32) {
// CHECK: fn get(&self) -> i32 {
// CHECK: fn set(&mut self, v: i32) {
// CHECK: fn origin() -> i32 {
// CHECK: fn op_eq(&self, o: &BoxI32) -> bool {
// CHECK: impl Noisy {
// CHECK: fn new(&mut self, i: i32) {
// CHECK: fn drop(&mut self) {
// CHECK: fn clone(&self) -> i32 {
// CHECK: impl Drop for Noisy {
// CHECK-NEXT: fn drop(&mut self) {
