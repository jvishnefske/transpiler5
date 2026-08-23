// REQUIRES: cargo
// W2.24: `try`/`throw`/`catch` as Result threading (a synthesized closed
// data enum, `ThrowsI32` here), end to end. THE oracle for the wave: the
// emitted crate's stdout AND stderr are diffed byte for byte against a
// `clang++ -std=c++17` build of the identical source — a compile-clean
// `cargo build` cannot see a dropped or misrouted exception, and every
// leg below is a CONTROL-FLOW divergence a FileCheck over IR could
// rationalize away. The two streams are captured to SEPARATE files and
// diffed separately (the cpp-iostream.cpp precedent): every admitted leg
// must leave stderr EMPTY on both sides, which is itself the pin that no
// exception escapes — the uncaught case, whose native/Rust stdio models
// measurably diverge, stays a located rejection
// (test/Import/Cpp/exceptions-invalid.cpp).
//
// What each leg is here to catch:
//  - A: propagation through THREE stack frames with NO throw taken — the
//    Ok0 threading must be an identity on the happy path.
//  - B: the same three frames WITH the throw taken at the bottom — one
//    early-return pyramid level per frame, payload intact at the handler.
//  - C: a throw from inside a LOOP inside a try — the branch out of the
//    loop to the handler must survive lift-cf-to-scf's structurization,
//    and the partial accumulator ok keeps exactly the iterations that ran.
//  - D: a NAMESPACED thrower (the planner's recursive walk, not the
//    TU-only scaffold) in statement position under catch(...) with the
//    payload discarded.
//  - E: a bare `throw;` RETHROW inside a catch, caught again one frame up.
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate. argv is
// unused (its use is a hard rejection).
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out 2> %t.native.err
// RUN: %t.crate/target/release/cpp_exceptions > %t.rust.out 2> %t.rust.err
// RUN: diff %t.native.out %t.rust.out
// RUN: diff %t.native.err %t.rust.err

extern "C" int printf(const char *, ...);

int level1(int x) {
  if (x < 2)
    throw x + 40;
  return x * 3;
}

int level2(int x) { return level1(x) + 1; }

int level3(int x) { return level2(x) + 1; }

namespace ns {
int nthrow(int x) {
  if (x > 0)
    throw x + 7;
  return x;
}
} // namespace ns

int rethrower(int x) {
  int r = 0;
  try {
    if (x < 5)
      throw x + 2;
    r = x;
  } catch (int e) {
    throw;
  }
  return r;
}

int main(int argc, char **) {
  int caught = 0;
  int ok = 0;

  // Leg A: depth 3, no throw (argc + 2 >= 3 clears the x < 2 gate).
  try {
    ok = level3(argc + 2);
  } catch (int e) {
    caught = e;
  }
  printf("A %d %d\n", ok, caught);

  // Leg B: depth 3, throw taken at the bottom (argc - 1 == 0 for the
  // standard run); the pending ok assignment must NOT happen, so ok keeps
  // leg A's value in the printf below.
  try {
    ok = level3(argc - 1);
  } catch (int e) {
    caught = e;
  }
  printf("B %d %d\n", ok, caught);

  // Leg C: throw from inside a loop in a try; ok keeps the partial sum.
  try {
    for (int i = 0; i < argc + 5; ++i) {
      if (i == argc + 2)
        throw i * 10;
      ok = ok + i;
    }
    ok = -1;
  } catch (int e) {
    caught = caught + e;
  }
  printf("C %d %d\n", ok, caught);

  // Leg D: namespaced thrower, statement position, catch(...) discards.
  try {
    ns::nthrow(argc);
    caught = -2;
  } catch (...) {
    caught = caught + 1000;
  }
  printf("D %d %d\n", ok, caught);

  // Leg E: rethrow inside a catch, caught again here.
  try {
    ok = rethrower(argc);
  } catch (int e) {
    caught = e;
  }
  printf("E %d %d\n", ok, caught);

  return 0;
}
