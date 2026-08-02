// REQUIRES: cargo
// FR-42 end-to-end oracle for recoverable import: a C++ translation unit
// that is PARTLY outside the supported subset. Without --recover the file
// yields nothing at all (the first rejection fails the whole compile, which
// the STRICT line below pins); with --recover the in-subset part still
// reaches a cargo crate that BUILDS, and its entry point still produces the
// right answer.
//
// The out-of-subset part is deliberately of both recoverable kinds:
//   - `scaled` rejects on a volatile local (C99-7) while its signature still
//     maps, so it becomes an `unimplemented!()` stub. Nothing calls it, so
//     the stub is never executed — but it must be present and well-formed,
//     because a crate that does not compile is not a partial result.
//   - `widen` rejects while its own signature is still being built
//     (`_Complex double` has no mapping), so it is dropped entirely, and
//     `useWiden`, whose only fault is calling it, is recovered in turn.
//
// The native leg compiles the SAME source with clang++ and only the
// in-subset behavior is compared, which is exactly the guarantee recovery
// offers: what survives is byte-identical to what C++ does.
//
// RUN: emitrust-cc --recover --emit=crate %s -o %t.crate --build 2>%t.err
// RUN: FileCheck %s --check-prefix=DIAG --input-file=%t.err
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/recover_partial > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

extern "C" int printf(const char *, ...);

// --- In subset: a class with a constructor, a mutating method and a const
// --- method, and a free function. All of this must survive intact.
class Accum {
public:
  Accum(int start) : total(start) {}
  void add(int d) { total = total + d; }
  int get() const { return total; }

private:
  int total;
};

int triple(int x) { return x * 3; }

// --- Out of subset, stubbable: the signature maps, the body does not.
int scaled(int x) {
  volatile int v = x;
  return v * 2;
}

// --- Out of subset, unmappable: dropped, taking its caller with it.
_Complex double widen(int x) { return (_Complex double)x; }

int useWiden(int x) {
  widen(x);
  return x;
}

int main() {
  Accum a(4);
  a.add(triple(5));
  a.add(2);
  printf("total=%d\n", a.get());
  printf("triple=%d\n", triple(7));
  return 0;
}

// Three recovered items, each located, each a warning; the compile exits 0.
// DIAG: warning: unsupported: volatile-qualified type (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG: warning: unsupported type '_Complex double' (recovered: item dropped)
// DIAG: warning: unsupported: call to unimported function 'widen' (recovered: emitted an unimplemented!() stub with the mapped signature)
// DIAG-NOT: error:
// DIAG: recovered 3 rejected top-level items:
// DIAG: stubbed 'scaled'
// DIAG: dropped 'widen'
// DIAG: stubbed 'use_widen'

// STRICT: error: unsupported: volatile-qualified type
// STRICT-NOT: warning:
