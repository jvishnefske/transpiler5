// RUN: split-file %s %t
// RUN: emitrust-import-c %t/chain.c | FileCheck %s --check-prefix=CHAIN
// RUN: emitrust-import-c %t/discrim.c | FileCheck %s --check-prefix=DISCRIM
// RUN: emitrust-import-c %t/swap.c | FileCheck %s --check-prefix=SWAP
// RUN: emitrust-import-c %t/self.c | FileCheck %s --check-prefix=SELF
// RUN: emitrust-import-c %t/cycle.c | FileCheck %s --check-prefix=CYCLE
// RUN: emitrust-import-c %t/perm.c | FileCheck %s --check-prefix=PERM
// RUN: emitrust-import-c %t/perm-rev.c | FileCheck %s --check-prefix=PERM

// FR-100: scalar out-parameter forwarding, CALLEE-AWARE. A pointer
// parameter that appears anywhere other than a direct dereference or
// arrow classifies as a Slice (collectSliceParams), and a bare
// forwarding reference in a call-argument position used to be one of
// those "anywhere else" appearances — so the C out-parameter idiom
// (`void inner(size_t *n); void outer(size_t *n) { inner(n); }`) forced
// `&mut [usize]` on a scalar and its `&n` call site took the located
// "address of a scalar object cannot be passed as a slice parameter"
// rejection. FR-92 already carved a POSITIONAL exception here for
// pointer-to-CONSTANT-ARRAY pointees, safe because such a pointee can
// never BE a slice; that reasoning does NOT transfer to arithmetic
// pointees, so this exception is CALLEE-AWARE instead: a forwarded
// arithmetic-pointee parameter keeps its scalar reference IFF the
// callee's corresponding parameter is itself a scalar reference.
//
// The answer is a TU-WIDE MONOTONE FIXPOINT over forwarding edges
// (seeds = the per-body local slice demands, each edge propagates a
// demand BACKWARD to stability), never an on-demand recursive query,
// so it is ORDER-INDEPENDENT: the two permuted arms below (PERM over
// perm.c and perm-rev.c) pin the SAME expectations on two definition
// orders of the same call graph, which is exactly the property an
// on-demand query could not give for a mutual-recursion cycle. Call
// sites forward the bare block argument (Rust's implicit reborrow),
// exactly as FR-92's arm does, and symbol-type equality is the proof
// that the callee agreed on the class.
//
// This file pins the ADMITTED shapes; the frontier that must keep the
// conservative Slice (non-arithmetic pointees, no in-TU definition,
// variadic callees, arity mismatches, indirect calls) is pinned in
// scalar-out-param-forward-invalid.c, and the differential that a
// SUBSCRIPTING callee still forces Slice backward onto its caller is
// pinned there and in array-2d-pointer-invalid.c's FWDSLICE arm.

// The out-parameter chain: `twice` only forwards, so both it and the
// leaf `bump` keep the scalar reference and the call operand is the
// bare block argument (twice, proving the arm is not single-use).
// CHAIN-LABEL: func.func @bump(
// CHAIN-SAME: %[[BO:.+]]: !emitrust.mut_ref<i32>
// CHAIN-LABEL: func.func @twice(
// CHAIN-SAME: %[[TO:.+]]: !emitrust.mut_ref<i32>
// CHAIN: call @bump(%[[TO]], {{.+}}) : (!emitrust.mut_ref<i32>, i32) -> ()
// CHAIN: call @bump(%[[TO]], {{.+}}) : (!emitrust.mut_ref<i32>, i32) -> ()
// CHAIN-LABEL: func.func @c_main(
// CHAIN: %[[A:.+]] = emitrust.addr_of mut
// CHAIN: call @twice(%[[A]]) : (!emitrust.mut_ref<i32>) -> ()

//--- chain.c
static void bump(int *out, int by) { *out += by; }
static void twice(int *out) { bump(out, 1); bump(out, 2); }
int main(void) { int n = 5; twice(&n); return n; }

// THE DISCRIMINATION (the motivating heatshrink `output_info` shape,
// flattened): in ONE call `fill` forwards BOTH a byte buffer and a
// scalar cursor. The buffer's callee subscripts it, so the Slice
// demand propagates backward and `out_buf` stays `&mut [u8]`; the
// cursor's callees only dereference it, so `output_size` stays
// `&mut u64`. A positional (non-callee-aware) exception could not tell
// these two apart.
// DISCRIM-LABEL: func.func @fill(
// DISCRIM-SAME: %[[BUF:.+]]: !emitrust.mut_ref<!emitrust.slice<ui8>>
// DISCRIM-SAME: %[[SZ:.+]]: ui64
// DISCRIM-SAME: %[[OUT:.+]]: !emitrust.mut_ref<ui64>
// DISCRIM: call @can_take(%{{.+}}, %[[OUT]]) : (ui64, !emitrust.mut_ref<ui64>) -> i32
// DISCRIM: call @push_byte(%{{.+}}, %{{.+}}, %[[OUT]], %{{.+}}) : (!emitrust.mut_ref<!emitrust.slice<ui8>>, ui64, !emitrust.mut_ref<ui64>, ui8) -> ()

//--- discrim.c
static void push_byte(unsigned char *buf, unsigned long buf_size,
                      unsigned long *output_size, unsigned char byte) {
  buf[(*output_size)++] = byte;
}
static int can_take(unsigned long buf_size, unsigned long *output_size) {
  return *output_size < buf_size;
}
static void fill(unsigned char *out_buf, unsigned long out_buf_size,
                 unsigned long *output_size) {
  unsigned char v = 0;
  while (can_take(out_buf_size, output_size)) {
    push_byte(out_buf, out_buf_size, output_size, (unsigned char)(v * 3 + 1));
    v++;
  }
}
int main(void) {
  unsigned char buf[64];
  unsigned long n = 0;
  fill(buf, sizeof(buf), &n);
  return (int)n;
}

// ARGUMENT-INDEX correctness: `f(p, q)` forwards its arguments SWAPPED
// into `g(a, b)`, whose `a` is dereferenced and whose `b` is
// subscripted. The edges must be index-aligned callerParam ->
// calleeParam, so `f`'s FIRST parameter inherits the slice demand and
// its SECOND stays scalar — the mirror image of `g`'s own shape. An
// index-blind edge would classify both the same way.
// SWAP-LABEL: func.func @g(
// SWAP-SAME: %[[GA:.+]]: !emitrust.mut_ref<i32>
// SWAP-SAME: %[[GB:.+]]: !emitrust.mut_ref<!emitrust.slice<i32>>
// SWAP-LABEL: func.func @f(
// SWAP-SAME: %[[FP:.+]]: !emitrust.mut_ref<!emitrust.slice<i32>>
// SWAP-SAME: %[[FQ:.+]]: !emitrust.mut_ref<i32>
// SWAP: call @g(%[[FQ]], %{{.+}}) : (!emitrust.mut_ref<i32>, !emitrust.mut_ref<!emitrust.slice<i32>>) -> ()

//--- swap.c
static void g(int *a, int *b) { *a = 1; b[0] = 2; b[1] = 3; }
static void f(int *p, int *q) { g(q, p); }
int main(void) {
  int x = 0;
  int arr[64];
  arr[0] = 0;
  f(arr, &x);
  return x + arr[1];
}

// A SELF-forwarding parameter with no local slice demand: the fixpoint
// seeds nothing, so the recursion adds nothing and the parameter stays
// scalar. (An on-demand recursive query would have to break this cycle
// by guessing.)
// SELF-LABEL: func.func @s(
// SELF-SAME: %[[SP:.+]]: !emitrust.mut_ref<i32>
// SELF: call @s(%[[SP]]) : (!emitrust.mut_ref<i32>) -> ()

//--- self.c
static void s(int *p) { if (*p) { *p -= 1; s(p); } }
int main(void) { int n = 3; s(&n); return n; }

// A 2-CYCLE of mutual forwarding: both parameters stay scalar, and
// which function the importer classifies first cannot change that.
// CYCLE-LABEL: func.func @a(
// CYCLE-SAME: %[[AP:.+]]: !emitrust.mut_ref<i32>
// CYCLE: call @b(%[[AP]]) : (!emitrust.mut_ref<i32>) -> ()
// CYCLE-LABEL: func.func @b(
// CYCLE-SAME: %[[BP:.+]]: !emitrust.mut_ref<i32>
// CYCLE: call @a(%[[BP]]) : (!emitrust.mut_ref<i32>) -> ()

//--- cycle.c
static void b(int *p);
static void a(int *p) { if (*p > 0) { *p -= 1; b(p); } }
static void b(int *p) { a(p); }
int main(void) { int n = 3; a(&n); return n; }

// ORDER INDEPENDENCE: the same two call graphs (a 3-level SUBSCRIPTING
// chain a1 -> b2 -> c3, and a 2-level DEREFERENCING chain sc1 -> sc2)
// under two permuted definition orders. Both files must classify
// identically — slice demand propagated backward through two
// forwarding edges for the first chain, scalar kept for the second —
// which is the whole reason the analysis is a fixpoint and not an
// on-demand query.
// (Order-agnostic checks: the definition order is the variable under
// test, so every line here is a -DAG.)
// PERM-DAG: func.func @a1(%{{.+}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.+}}: i32)
// PERM-DAG: func.func @b2(%{{.+}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.+}}: i32)
// PERM-DAG: func.func @c3(%{{.+}}: !emitrust.mut_ref<!emitrust.slice<i32>>, %{{.+}}: i32)
// PERM-DAG: func.func @sc1(%{{.+}}: !emitrust.mut_ref<i32>)
// PERM-DAG: func.func @sc2(%{{.+}}: !emitrust.mut_ref<i32>)
// PERM-DAG: call @sc2(%arg0) : (!emitrust.mut_ref<i32>) -> ()

//--- perm.c
static void a1(int *v, int n);
static void b2(int *v, int n);
static void c3(int *v, int n) { v[1] = n; }
static void a1(int *v, int n) { b2(v, n); }
static void b2(int *v, int n) { c3(v, n); }
static void sc2(int *v) { *v += 1; }
static void sc1(int *v) { sc2(v); }
int main(void) {
  int arr[64];
  int x = 0;
  arr[1] = 0;
  a1(arr, 7);
  sc1(&x);
  return arr[1] + x;
}

//--- perm-rev.c
static void sc2(int *v);
static void c3(int *v, int n);
static void sc1(int *v) { sc2(v); }
static void sc2(int *v) { *v += 1; }
static void b2(int *v, int n) { c3(v, n); }
static void a1(int *v, int n) { b2(v, n); }
static void c3(int *v, int n) { v[1] = n; }
int main(void) {
  int arr[64];
  int x = 0;
  arr[1] = 0;
  a1(arr, 7);
  sc1(&x);
  return arr[1] + x;
}
