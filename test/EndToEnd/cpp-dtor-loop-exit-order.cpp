// REQUIRES: cargo
// FR-193 item 6 / W2.17: the RELATIVE ORDER of a loop-body local's destructor
// and the side effects on the loop's EXIT paths.
//
// `lift-cf-to-scf` structurizes the CFG. A block that is lexically inside a
// loop body but from which every path LEAVES the loop is not part of the
// cycle, so the lift places it AFTER the loop and dispatches it on an exit
// index. For plain C that is unobservable. For C++ it is a miscompile: the
// loop-body local's `Drop` runs at the end of the emitted loop-body region,
// so a `printf` that C++ runs BEFORE the destructor gets emitted AFTER it.
// Measured, before this was fenced:
//
//   native : body 0 / dtor 0 / body 1 / before break / dtor 1 / after loop
//   emitted: body 0 / dtor 0 / body 1 / dtor 1 / before break / after loop
//
// That exact shape is now a located rejection (see
// test/Driver/cpp-dtor-loop-exit-reject.cpp). THIS file pins the other side
// of the fence: the loop-exit shapes that were MEASURED byte-identical to
// the clang native and must keep working, so the fence stays a scalpel and
// not a ban on `break`/`return` near a destructor.
//
// Every shape here is a runtime destructor TRACE: a FileCheck of the IR
// cannot see an ordering bug, only a byte-diff of stdout can. Every count and
// id derives from argc so constant folding cannot pre-compute the answers and
// hide a miscompile behind a compile-clean crate.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_dtor_loop_exit_order > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

// A side-effecting loop condition: it keeps the always-exiting loop below
// from folding away, so the shape is measured and not optimised out.
static int probe(int i) {
  printf("cond %d\n", i);
  return i;
}

struct T {
  int id;
  T(int i) : id(i) { printf("ctor %d\n", id); }
  ~T() { printf("dtor %d\n", id); }
};

// `continue` stays INSIDE the cycle -- its block reaches the latch -- so the
// side effect ahead of it is not relocated and the per-iteration drop still
// follows it.
static void continueShape(int n) {
  printf("-- continue\n");
  for (int i = 0; i < n; ++i) {
    T a(i);
    if (i == 1) {
      printf("before continue %d\n", i);
      continue;
    }
    printf("tail %d\n", i);
  }
  printf("after continue\n");
}

// A `break` with NOTHING observable on the exit path: the relocation is real
// but carries no side effect across the drop, so the trace is identical.
static void cleanBreakShape(int n) {
  printf("-- clean break\n");
  for (int i = 0; i < n; ++i) {
    T a(i);
    printf("body %d\n", i);
    if (i == 1)
      break;
  }
  printf("after clean break\n");
}

// The side effect sits BEFORE the branch decision, so its block still reaches
// the latch and stays in the loop -- ahead of the drop, as C++ requires.
static void effectBeforeDecisionShape(int n) {
  printf("-- effect before decision\n");
  for (int i = 0; i < n; ++i) {
    T a(i);
    printf("g %d\n", i);
    if (i == 1)
      break;
    printf("h %d\n", i);
  }
  printf("after effect before decision\n");
}

// A `break` inside a `switch` targets the SWITCH, not the loop, so nothing
// leaves the cycle: the case body is not relocated.
static void switchBreakShape(int n) {
  printf("-- switch break\n");
  for (int i = 0; i < n; ++i) {
    T a(i);
    switch (i) {
    case 1:
      printf("case one\n");
      break;
    default:
      printf("case other %d\n", i);
      break;
    }
    printf("tail %d\n", i);
  }
  printf("after switch break\n");
}

// A `switch` case that RETURNS does leave the loop -- but nothing observable
// precedes it inside the case, and the `default` arm rejoins the cycle, so
// nothing crosses the drop. This is the accepted twin of the rejected
// `case 1: printf(..); return i;`, and it pins that the label-unwrapping fix
// did not turn every switch-with-a-return into a rejection.
static int switchCaseReturnShape(int n) {
  printf("-- switch case return\n");
  for (int i = 0; i < n; ++i) {
    T a(i);
    switch (i) {
    case 1:
      return i + 5;
    default:
      printf("other %d\n", i);
      break;
    }
  }
  return -1;
}

// Nested loops: the INNER `break` leaves the inner loop only. The outer
// body's local `o` outlives that relocation, so its drop still lands after
// every inner effect. (C++ has no `break` that targets an outer loop; the
// flag spelling that emulates one is the rejected shape, pinned in the
// Driver test.)
static void nestedInnerBreakShape(int n) {
  printf("-- nested inner break\n");
  for (int i = 0; i < n; ++i) {
    T o(100 + i);
    for (int j = 0; j < n + 2; ++j) {
      printf("inner %d %d\n", i, j);
      if (j == 1) {
        printf("inner exit %d\n", i);
        break;
      }
    }
    printf("outer tail %d\n", i);
  }
  printf("after nested inner break\n");
}

// TWO destructors whose RELATIVE order is observable: one declared BEFORE the
// loop and one INSIDE it. C++ drops the inner one per iteration and the outer
// one at the end of the function body, after everything the loop printed --
// including the code the lift relocated past the loop.
static void outerAndInnerLocalShape(int n) {
  printf("-- outer and inner local\n");
  T outer(900);
  for (int i = 0; i < n; ++i) {
    T a(i);
    printf("body %d\n", i);
    if (i == 1)
      break;
  }
  printf("after outer and inner local\n");
}

// A plain `while` CONDITION with a side effect is the accepted twin of the
// rejected do-while one: C++ evaluates it at the TOP of each iteration, after
// the previous iteration's drop, and the emitted loop agrees. Only the
// do-while spelling -- where the condition runs after the body, at the latch
// -- moves the effect across the drop.
static void whileConditionShape(int n) {
  printf("-- while condition\n");
  int i = 0;
  while (probe(i) < n) {
    T a(i);
    printf("body %d\n", i);
    ++i;
  }
  printf("after while condition\n");
}

// A loop body that ALWAYS leaves the loop has no back edge, so there is no
// cycle for anything to be relocated out of: the side effect before the
// `return` stays put. The side-effecting condition keeps the loop from
// folding away entirely.
static int unconditionalReturnShape(int n) {
  printf("-- unconditional return\n");
  int i = 0;
  while (probe(i) < n) {
    T a(i);
    printf("only %d\n", i);
    return i + 7;
  }
  return -1;
}

int main(int argc, char **) {
  int n = argc + 2;
  continueShape(n);
  cleanBreakShape(n);
  effectBeforeDecisionShape(n);
  switchBreakShape(n);
  printf("sr=%d\n", switchCaseReturnShape(n));
  nestedInnerBreakShape(argc);
  outerAndInnerLocalShape(n);
  whileConditionShape(argc + 1);
  printf("r=%d\n", unconditionalReturnShape(argc + 40));
  printf("done\n");
  return 0;
}
