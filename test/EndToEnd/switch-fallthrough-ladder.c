// REQUIRES: cargo
// FR-198 differential test for the guarded-ladder lowering of a C switch
// whose cases fall through in a chain.
//
// A fall-through chain has no properly nested structured equivalent, so
// `lift-cf-to-scf` duplicated the tail into every arm (case k emitted about
// (N - k + 1) times, O(N^2.7) output, >360s and no output at all at N=100 --
// FR-197). The importer now recognizes the shape on the clang AST and emits
// the linear equivalent instead: the controlling expression selects a section
// INDEX and the sections run under `entry <= k` guards.
//
// That rewrite is only correct if three hazards are handled, and every one of
// them is exercised below against the clang-built native binary:
//   1. DECLARATION SCOPE. A declaration at switch-body scope stays live for
//      LATER cases in C, but each guarded `if` is its own Rust scope, so the
//      shape is REFUSED and keeps the old lowering (`decl_carried`,
//      `decl_tail`).
//   2. SINGLE EVALUATION. The controlling expression must be evaluated
//      exactly once even when it has side effects (`side_effect`, whose
//      global call counter is printed).
//   3. `break`. A `break` targeting this switch stops the chain, so the shape
//      is refused (`mid_break`); a `break` belonging to a nested loop or
//      switch does NOT (`nested_loop_break`, `nested_switch`), and a `break`
//      that is the final statement of the final section is the no-op fall-out
//      and is admitted (every `default: break;` here).
// `goto_out` additionally pins that a `goto` leaving the switch is refused,
// and `with_return` / `in_loop` that `return` and `continue` inside a case
// abandon the rest of the chain exactly as they abandon the rest of the
// switch in C.
//
// Every scrutinee derives from `argc` so no constant fold can evaluate a
// switch at compile time and hide a miscompile. Byte-identical stdout and
// exit codes against the clang-built native binary are required.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/switch_fallthrough_ladder > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

int g_calls = 0;
int bump(int d) {
  g_calls += 1;
  return d;
}

/* The plain ladder: dense labels, every arm falls into the next, trailing
   `default: break;`. */
int ladder(int x) {
  int acc = 0;
  switch (x) {
  case 0: acc += 1;
  case 1: acc += 2;
  case 2: acc += 4;
  case 3: acc += 8;
  case 4: acc += 16;
  case 5: acc += 32;
  case 6: acc += 64;
  case 7: acc += 128;
  default: break;
  }
  return acc;
}

/* Sparse, negative and out-of-order labels with `default:` in the MIDDLE of
   the body: the entry index is a section POSITION, not a case value. */
int sparse(int x) {
  int acc = 0;
  switch (x) {
  case -7: acc += 1;
  case 1000: acc += 2;
  default: acc += 4;
  case -1: acc += 8;
  case 0: acc += 16;
  }
  return acc;
}

/* No `default:` at all: an unmatched value must run NOTHING. */
int no_default(int x) {
  int acc = 0;
  switch (x) {
  case 1: acc += 1;
  case 2: acc += 2;
  case 3: acc += 4;
  }
  return acc;
}

/* The last section returns; the earlier ones fall into it. */
int with_return(int x) {
  int acc = 0;
  switch (x) {
  case 0: acc += 1;
  case 1: acc += 2;
  case 2: return acc + 100;
  }
  return acc;
}

/* Hazard 2: the controlling expression is evaluated exactly once. */
int side_effect(int d) {
  int acc = 0;
  switch (bump(d)) {
  case 0: acc += 1;
  case 1: acc += 2;
  case 2: acc += 4;
  default: break;
  }
  return acc * 10 + g_calls;
}

/* A `break` inside a nested loop belongs to the LOOP, so the outer chain is
   still a ladder. */
int nested_loop_break(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    while (acc < 20) {
      acc += 3;
      if (acc > 10) break;
    }
  case 2:
    acc += 100;
  default:
    break;
  }
  return acc;
}

/* A complete inner switch (with its own breaks) inside a ladder arm. */
int nested_switch(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    switch (acc & 1) {
    case 0: acc += 10; break;
    default: acc += 20; break;
    }
  case 2:
    acc += 100;
  default:
    break;
  }
  return acc;
}

/* `continue` inside a case abandons the rest of the chain and the rest of the
   loop body, exactly as it does in C. */
int in_loop(int n) {
  int acc = 0;
  for (int i = 0; i < n; ++i) {
    switch (i % 4) {
    case 0:
      acc += 1;
    case 1:
      if (i == 3) continue;
      acc += 2;
    case 2:
      acc += 4;
    default:
      break;
    }
    acc += 1000;
  }
  return acc;
}

/* Hazard 1: a declaration at switch-body scope that a LATER case reads. The
   shape must be refused; only the entry that runs the initializer reads it,
   so the program stays defined. */
int decl_carried(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    ;
    int carried = 7;
    acc += carried;
  case 1:
    acc += 1;
  default:
    acc += 2;
  }
  return acc;
}

/* The same hazard in the FINAL section, where nothing follows it. Refused
   just the same -- the rule is syntactic, not a liveness analysis. */
int decl_tail(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    acc += 2;
  default:
    ;
    int tail = 5;
    acc += tail;
  }
  return acc;
}

/* Hazard 3: a `break` in a non-final section stops the chain. Refused. */
int mid_break(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
    break;
  case 1:
    acc += 2;
  case 2:
    acc += 4;
  default:
    acc += 8;
  }
  return acc;
}

/* A `goto` leaving the switch. Refused. */
int goto_out(int x) {
  int acc = 0;
  switch (x) {
  case 0:
    acc += 1;
  case 1:
    acc += 2;
    if (acc > 2) goto done;
  case 2:
    acc += 4;
  default:
    acc += 8;
  }
done:
  return acc;
}

unsigned uladder(unsigned x) {
  unsigned acc = 0u;
  switch (x) {
  case 0u: acc += 1u;
  case 7u: acc += 2u;
  case 4000000000u: acc += 4u;
  default: break;
  }
  return acc;
}

long lladder(long x) {
  long acc = 0;
  switch (x) {
  case -1: acc += 1;
  case 3: acc += 2;
  case 5000000000L: acc += 4;
  default: break;
  }
  return acc;
}

enum E { EA = -3, EB = 0, EC = 9 };
int eladder(enum E e) {
  int acc = 0;
  switch (e) {
  case EA: acc += 1;
  case EB: acc += 2;
  case EC: acc += 4;
  }
  return acc;
}

int main(int argc, char **argv) {
  /* Seeds derived from argc: 1 when run with no arguments, but opaque to the
     optimizer, so nothing below can be constant-folded away. */
  int s = argc;
  for (int i = -9; i < 12; ++i) {
    int v = i * s;
    printf("%d %d %d %d %d\n", ladder(v), sparse(v), no_default(v),
           with_return(v), nested_loop_break(v));
    printf("%d %d %d %d %d\n", nested_switch(v), decl_carried(v),
           decl_tail(v), mid_break(v), goto_out(v));
    printf("%u %ld %d\n", uladder((unsigned)(v + 7 * s)), lladder((long)v),
           eladder((enum E)(v - 3 * s)));
  }
  for (int i = 0; i < 12; ++i)
    printf("loop %d\n", in_loop(i * s));
  for (int i = -2; i < 5; ++i)
    printf("se %d\n", side_effect(i * s));
  printf("calls %d\n", g_calls);
  printf("sparse1000 %d ladder1000 %d\n", sparse(1000 * s), ladder(4 * s));
  return 0;
}
