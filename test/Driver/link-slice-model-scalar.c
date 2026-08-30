// FR-161 phase 1: the ELEMENT-ZERO out-parameter, the residue FR-158
// phases 1+2 left behind.
//
// FR-158 reconciles a cross-shard slice-model divergence by re-shaping the
// declaring shard's `&arr[k]` cursor into `&mut arr[k..]`. That rewrite
// needs a REGION to slice, so `f(&x)` -- the address of a scalar local or
// of a struct field -- had no form and stayed a located rejection: 60
// argument slots over 55 caller functions on the 501-object systemd link,
// every one of them the C out-parameter idiom, and the only thing left
// stopping that link from emitting a crate.
//
// The form this file pins: the argument is wrapped in
// `::std::slice::from_mut`, the standard-library one-element slice view, so
// the callee receives exactly the one-element region C's `&x` lends it.
// Two spellings are load-bearing and both are measured, not chosen:
//   * `::std::`, NOT `::core::` -- `::core::slice::from_mut` resolves under
//     cargo but is E0433 under bare `rustc`, which the lit EndToEnd tests
//     use directly;
//   * the LEADING `::` -- FR-159 sinks items into `mod tu<N>`, where a
//     relative `std::` can be shadowed by a TU-local item.
//
// THE FENCE is the whole reason this is shippable, and it is what this file
// exists to hold still. A one-element slice makes `p[1]` a PANIC where C
// merely had undefined behaviour, and this project has already decided that
// direction twice -- FR-75 deliberately flipped the accepted `helper(&x, 1)`
// shape to the located rejection, and `pointers-param-invalid.c` pins
// `int first(int *a){return a[0]+a[1];}` called as `first(&x)` as a
// rejection. So the wrap is admitted ONLY when the DEFINITION's own
// parameter provably touches element 0 and nothing else. Measured: without
// the fence, C prints `1 2` and the crate panics `index out of bounds: the
// len is 1 but the index is 1`.
//
// The fence must be TRANSITIVE, and that is not a nicety: 3 of the 60
// systemd slots (`parse_sec`, `pidfd_get_pid`, `read_attr_at`) FORWARD the
// parameter into another slice-parameter callee (`parse_sec` hands
// `&mut (*ret)[0..]` to `parse_time`), and a shallow fence rejects them --
// after which the crate does not emit at all. The FWD leg below is that
// shape.
//
// Durability, deliberately not claimed to be stable: 28 of the 30 admitted
// systemd callees pass the fence because they are today FR-52
// `unimplemented!` stubs with no uses. As later waves give them bodies the
// fence will re-reject some. That is the correct direction -- a located
// rejection, never a panic.
//
// The legs:
//   ZERO -- `setv` writes `p[0]` only. A scalar local (`&x`) AND a struct
//           field (`&s.a`), the exact 50/10 split of the systemd census.
//           Both wrap; the link succeeds.
//   FWD  -- `outer` never subscripts its own parameter; it forwards
//           `&mut (*ret)[0..]` to `inner`, which writes element 0. Admitted
//           only because the fence follows the forward.
//   NZ   -- `first` reads `a[0] + a[1]`. Declines to the EXISTING FR-158
//           located rejection, verbatim, so the fence cannot silently
//           loosen into the shape FR-75 rejected.
//
// The per-TU `-D` is load-bearing: with identical import args FR-58's joint
// link-time RE-IMPORT reconciles these pairs in the importer and none of
// the merge-level code under test runs.

// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/def-zero.c -DTU_A=1 -o %t/def-zero.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-zero.c -DTU_B=1 -o %t/use-zero.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-nonzero.c -DTU_C=1 -o %t/use-nonzero.o

// ZERO + FWD: the link succeeds and every diverging argument is wrapped.
// RUN: emitrust-cc --link %t/def-zero.o %t/use-zero.o --crate-type=lib --emit=rust -o %t/zero.rs
// RUN: FileCheck %s --check-prefix=ZERO < %t/zero.rs
// RUN: FileCheck %s --check-prefix=ZERONOT < %t/zero.rs

// The definition side is untouched: `outer` still forwards a tail borrow of
// the whole region it was lent, which is what the fence had to follow.
// ZERO:      let {{v[0-9]+}}: &mut [u32] = &mut (*ret)[0i64 as usize..];
// ZERO:      pub fn zero_caller
// A scalar LOCAL -- 50 of the 60 measured systemd slots.
// ZERO:      let [[X:v[0-9]+]]: &mut u32 = &mut x;
// ZERO-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[X]]);
// ZERO-NEXT: = setv(
// A struct FIELD -- the other 10.
// ZERO:      let [[A:v[0-9]+]]: &mut u32 = &mut s.a;
// ZERO-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[A]]);
// ZERO-NEXT: = setv(
// FWD -- `outer`'s own argument wraps too, on the strength of `inner`.
// ZERO:      let [[Y:v[0-9]+]]: &mut u32 = &mut y;
// ZERO-NEXT: let {{v[0-9]+}}: &mut [u32] = ::std::slice::from_mut([[Y]]);
// ZERO-NEXT: = outer(

// Never the relative spelling (FR-159 sinks items into `mod tu<N>`, where a
// TU-local item can shadow it) and never `core` (E0433 under bare rustc).
// ZERONOT-NOT: core::slice::from_mut
// ZERONOT-NOT: = std::slice::from_mut

// The NZ leg -- a definition that reads element 1 keeps the FR-158
// rejection, so the fence cannot silently loosen into the shape FR-75
// refused.
// RUN: not emitrust-cc --link %t/def-zero.o %t/use-nonzero.o --crate-type=lib --emit=rust -o %t/nonzero.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NZ
// NZ: use-nonzero.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument 1 of the call to 'first' is a scalar reference but the defining translation unit classifies that parameter as a slice
// NZ: def-zero.c:{{[0-9]+}}:{{[0-9]+}}: note: 'first' is defined here as '(!emitrust.mut_ref<!emitrust.slice<i32>>, i32) -> i32'

//--- def-zero.c
/* Every parameter here is subscripted, so the importer classifies all four
   as slices; only the INDEX distinguishes them. */
int setv(unsigned *p, unsigned v) {
  p[0] = v;
  return 0;
}

int inner(unsigned *r, unsigned v) {
  r[0] = v;
  return 0;
}

/* `outer` never touches an element itself -- it hands the whole tail to
   `inner`. A non-transitive fence sees only the `slice_of` and refuses. */
int outer(unsigned *ret, unsigned v) { return inner(ret, v); }

/* Element 1: the fence must refuse this one. */
int first(int *a, int n) { return a[0] + a[1] + n; }

//--- use-zero.c
struct S {
  unsigned a;
  unsigned b;
};

int setv(unsigned *, unsigned);
int outer(unsigned *, unsigned);

int zero_caller(void) {
  unsigned x = 99;
  unsigned y = 0;
  struct S s;
  s.a = 7;
  s.b = 8;
  setv(&x, 11);
  setv(&s.a, 22);
  outer(&y, 33);
  return (int)(x + s.a + s.b + y);
}

//--- use-nonzero.c
int first(int *, int);

int nonzero_caller(void) {
  int x = 3;
  return first(&x, 1);
}
