// FR-193 item 6 / W2.17: the LOOP-EXIT drop-order fence, and the exact
// wording it says it with.
//
// `lift-cf-to-scf` relocates a block from which every path leaves the loop to
// AFTER the loop, dispatched on an exit index. A loop-body local's Rust
// `Drop` runs at the end of the emitted loop-body region, so an effect C++
// runs BEFORE the destructor is emitted AFTER it. Both repros below built
// clean and printed the trace in the wrong ORDER -- precisely the class
// W2.17 and FR-111 exist to fence:
//
//   break : native  body 0 / dtor 0 / body 1 / before break / dtor 1
//           emitted body 0 / dtor 0 / body 1 / dtor 1 / before break
//   return: native  dtor 0 / before return / dtor 1 / r=1
//           emitted dtor 0 / dtor 1 / before return / r=1
//
// Rejection is a feature: each shape gets a LOCATED error AT THE DECLARATION
// -- the object whose drop moves -- plus a note AT THE EFFECT that moved,
// because naming the loop is not enough to act on. The ACCEPTED side of the
// fence (`continue`, a break with nothing observable on its exit path, an
// effect ahead of the decision, a `switch` case's break, an inner loop's
// break, a body that always leaves) is byte-diffed against the clang native
// in test/EndToEnd/cpp-dtor-loop-exit-order.cpp; a fence that swallowed
// those would be a bigger regression than the defect.
//
// The plain run pins that the FIRST shape fails the whole compile loudly; the
// recovering run pins every shape at once, plus the FR-42 ledger tag they
// share with the rest of the W2.17 scope family.
// RUN: not emitrust-cc --emit=rust %s -o - 2>&1 | FileCheck --check-prefix=PLAIN %s
// RUN: emitrust-cc --emit=rust --recover %s -o /dev/null 2>&1 | FileCheck %s

extern "C" int printf(const char *, ...);

struct T {
  int id;
  T(int i) : id(i) {}
  ~T() { printf("dtor %d\n", id); }
};

// Repro 1: the `break`. The `printf` block only ever leaves the loop.
int breakShape(int n) {
  for (int i = 0; i < n; ++i) {
    // PLAIN: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+2]]:{{[0-9]+}}: error: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    printf("body %d\n", i);
    if (i == 1) {
      // PLAIN: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+2]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++, but it is on a path that only leaves the loop, so it is emitted after the loop -- past the drop
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++, but it is on a path that only leaves the loop, so it is emitted after the loop -- past the drop
      printf("before break\n");
      break;
    }
  }
  return 0;
}

// Repro 2: the `return`. Same relocation, different terminator.
int returnShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    if (i == 1) {
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      printf("before return\n");
      return i;
    }
  }
  return -1;
}

// The return's OPERAND counts: C++ evaluates it and THEN destroys the local,
// while the emitted code carries it out with the rest of the exit path.
int side(int x) {
  printf("side %d\n", x);
  return x;
}
int returnOperandShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    if (i == 1)
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      return side(i);
  }
  return -1;
}

// A `switch` case that RETURNS leaves the loop; only its `break` is caught by
// the switch. The analysis' over-approximation was measured WRONG exactly
// here -- it treated the switch as always-leaving and skipped this shape,
// which kept printing `case one` after `dtor 1`.
int switchCaseReturnShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    switch (i) {
    case 1:
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      printf("case one\n");
      return i;
    default:
      break;
    }
  }
  return -1;
}

// The same switch defect one label deeper: the `return` is the SUB-STATEMENT
// of a later case label, not a sibling of the earlier one. A first attempt
// unwrapped the label by first CHILD, which for a `CaseStmt` is the label
// EXPRESSION -- so `case 1: return 1;` answered "does not leave the loop" and
// the shape kept miscompiling (`zero 0` printed after `dtor 0`). The wrapper
// is unwrapped by NAME now, and this pins it.
int caseSubStmtReturnShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    switch (i) {
    case 0:
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      printf("zero %d\n", i);
    case 1:
      return 1;
    default:
      break;
    }
  }
  return -1;
}

// Nested loops. C++ has no `break` that targets an OUTER loop, so the shape
// is spelled with a flag; the OUTER body's local is the one whose drop the
// relocated inner-exit effects cross, and it is the declaration named.
int nestedShape(int n) {
  int stop = 0;
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T o(100 + i);
    for (int j = 0; j < n; ++j) {
      T b(10 * i + j);
      if (j == 1) {
        printf("inner exit %d %d\n", i, j);
        stop = 1;
        break;
      }
    }
    if (stop) {
      // The note names the OUTER loop's own exit path: the inner `break`
      // stays inside the outer cycle, so it is the flag test that carries
      // `o`'s drop across a relocation.
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      printf("outer exit %d\n", i);
      break;
    }
  }
  return 0;
}

// A local declared BEFORE the loop plus one INSIDE it: the relative order of
// two destructors is what makes the relocation observable, and the INNER one
// is the object whose drop the exit path crosses.
int outerAndInnerShape(int n) {
  T outer(900);
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    T a(i);
    if (i == 1) {
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      printf("before break\n");
      break;
    }
  }
  return 0;
}

// A relocated GLOBAL STORE with no call at all is the same defect: the
// destructor reads `counter` and sees the pre-store value (measured
// `dtor 1 gx=0` where the native prints `dtor 1 gx=7`). The `for`-increment
// gate's call-only screen would have missed this one.
int counter = 0;
struct G {
  int id;
  G(int i) : id(i) {}
  ~G() { printf("gdtor %d %d\n", id, counter); }
};
int globalStoreShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose exit path has side effects
    G g(i);
    if (i == 1) {
      // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: note: this side effect runs before the destructor in C++
      counter = 7;
      break;
    }
  }
  return counter;
}

// THE LATCH FAMILY, found while measuring the exit paths and fenced with
// them: code that C++ runs AFTER the body's locals are destroyed, but that
// the emitted loop renders at the bottom of the body, AHEAD of the drop.
//
// A do-while CONDITION is the `for` increment's twin, and only the increment
// was gated. Measured: `body 0 / dtor 0 / cond 1` came out
// `body 0 / cond 1 / dtor 0`. A plain `while` is NOT affected -- its
// condition runs at the top of the iteration, after the previous drop -- and
// stays accepted, byte-diffed in the EndToEnd twin.
int side2(int x) {
  printf("cond %d\n", x);
  return x;
}
int doWhileConditionShape(int n) {
  int i = 0;
  do {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a do-while loop whose condition has side effects
    T a(i);
    printf("body %d\n", i);
    ++i;
  } while (side2(i) < n);
  return i;
}

// And the increment gate itself screened only for CALLS, so a global STORE
// walked straight through it. Measured: the destructor printed
// `dtor 0 tick=1` where the native prints `dtor 0 tick=0`.
int tick = 0;
struct K {
  int id;
  K(int i) : id(i) {}
  ~K() { printf("kdtor %d %d\n", id, tick); }
};
int forIncrementStoreShape(int n) {
  for (int i = 0; i < n; ++i, tick++) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor in a loop whose increment has side effects
    K a(i);
    printf("body %d\n", i);
  }
  return tick;
}

// The `goto` sibling was ALREADY fenced, one gate earlier and for a different
// reason (a function carrying a label hoists its locals to function top), and
// that wording must not change: this fence must not steal shapes from it.
int gotoShape(int n) {
  for (int i = 0; i < n; ++i) {
    // CHECK: cpp-dtor-loop-exit-reject.cpp:[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: object of a class with a destructor outside a function, loop, or branch body
    T a(i);
    if (i == 1) {
      printf("before goto\n");
      goto done;
    }
  }
done:
  return 0;
}

// FR-42: the whole family lands under the SAME ledger tag as the rest of the
// W2.17 scope gates, so the backlog keeps ranking it as one construct.
// CHECK: recovered 11 rejected top-level items
// CHECK: cxx-drop-scope 11

int main(int argc, char **) {
  return breakShape(argc) + returnShape(argc) + returnOperandShape(argc) +
         switchCaseReturnShape(argc) + caseSubStmtReturnShape(argc) +
         nestedShape(argc) +
         outerAndInnerShape(argc) + globalStoreShape(argc) +
         doWhileConditionShape(argc) + forIncrementStoreShape(argc) +
         gotoShape(argc);
}
