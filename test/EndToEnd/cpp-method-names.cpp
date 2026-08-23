// REQUIRES: cargo
// FR-110: behavior oracle for the method-name strip, deliberately built
// around the one collision surface the strip CREATES: a droppy class with
// user methods literally spelled `drop` and `clone` now renders INHERENT
// `fn drop`/`fn clone` beside `impl Drop` and `derive(Clone)` machinery.
// Rust resolves the explicit call sites to the inherent methods while RAII
// still runs `Drop::drop`, so the printed interleaving (user drop lines
// vs. dtor lines, in program order) is the exact thing a misbinding would
// corrupt -- and the byte-diff against the clang++ native would see it.
// Also drives the ordinary strips (ctor `new`, `get`/`set`, static
// `origin`, operator `op_eq`) through data-dependent values: every value
// derives from argc, so no constant folding can pre-compute the answers
// and hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_names > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

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
  int clone() const { return id + 7; }
  ~Noisy() { printf("bye %d\n", id); }
};

int main(int argc, char **argv) {
  BoxI32 bi(argc);
  bi.set(argc + 2);
  int v = bi.get();
  BoxI32 b2(argc + 2);
  int same = (bi == b2) ? 1 : 0;
  BoxI32 b3(argc + 3);
  int diff = (bi == b3) ? 1 : 0;
  int o = BoxI32::origin();
  printf("box %d %d %d %d\n", v, same, diff, o);
  Noisy n(argc);
  n.drop();
  n.drop();
  int c = n.clone();
  printf("cloned %d\n", c);
  Noisy m(argc + 10);
  m.drop();
  printf("done %d\n", v + c);
  return 0;
}
