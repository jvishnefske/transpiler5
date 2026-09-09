// FR-209: a `--c-abi-exports` refusal must name the reason that ACTUALLY
// decided it. This file pins the one slice class whose cause is invisible in
// the signature the author wrote, and -- more importantly -- pins that every
// other refusal keeps FR-139's original sentence verbatim.
//
// THE DEFECT. FR-100's forwarding fixpoint keeps a pointer parameter as a
// scalar reference across a forwarding call ONLY when the pointee is
// ARITHMETIC (CImporterInternal.h, `collectSliceParamsImpl`); a STRUCT
// pointee records no forwarding edge and falls through to the conservative
// slice demand, so the parameter is emitted `&mut [T]` and every FR-182 /
// FR-202 export class refuses it. What the author saw was:
//
//   no C-ABI export for 'outer': its signature is not all-scalar (a C-ABI
//   entry point may only take and return builtin integer and floating-point
//   types)
//
// which is factually true and completely uninformative. It names neither the
// delegation nor the pointee-kind rule. `int outer(Ctx *c)` is a signature
// with nothing wrong with it, and the sibling `inner(Ctx *c)` -- the SAME
// signature -- exports. The person reading that has been told to fix a
// signature that is not the problem.
//
// THE DIFFERENTIAL, and it is the whole point: three functions of the
// IDENTICAL C signature `int f(Ctx *)`, differing only in what the body does
// with the pointer.
//
//   `inner`  member access only         -> `&mut Ctx`,   EXPORTED
//   `outer`  forwards the whole pointer -> `&mut [Ctx]`, REFUSED
//   `solo`   control, member access     -> `&mut Ctx`,   EXPORTED
//
// Two functions export and one does not, from one signature. No sentence
// about the signature can ever explain that, which is exactly why the
// refusal now names the CALL instead, with a located note on the line that
// performed the delegation -- the only line the author can act on.
//
// SCOPE, deliberately narrow. This changes WORDING and not ADMISSION:
// `outer` is refused before and after, and the emitted crate is unchanged.
// Widening the forwarding edge to record struct pointees would move emitted
// signatures repo-wide and is NOT this increment (recorded as uncosted in
// FR-209).
//
// THE REGRESSION GUARD MATTERS MORE THAN THE NEW TEXT. FR-139's sentence is
// correct for a genuinely unscalar signature and must not drift for one:
// `by_value` (a struct by value with a pointer member), `walks` (a real
// subscripted slice, the FR-138 shape) and `both` (which forwards AND
// subscripts, so the delegation is NOT its sole reason) all keep the wording
// they had, word for word. A `walks`-style slice is a two-register fat
// pointer that shifts every later argument -- measured on crc16, native
// 21983 against export 25322, exit 0, no diagnostic -- and nothing about
// that reasoning changed, so neither does its sentence.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// The two same-signature siblings really do export, so the refusal above is
// about the delegation and not about `Ctx` being unexportable.
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
//
// `outer` gains NO C surface of any kind: rejection is a feature and the
// wording change must not have quietly admitted it.
// RUN: not grep 'cabi_outer' %t.cabi/src/lib.rs
// RUN: not grep 'export_name = "outer"' %t.cabi/src/lib.rs
//
// And without the flag nothing moves at all -- the whole feature is behind
// a default-OFF flag, so the flagless crate is byte-identical apart from the
// four `--c-abi-exports` additions. Checked by DIFF on the warning stream.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err

typedef struct Ctx {
  int a;
  int b;
} Ctx;

// The helper. Its own parameter is a plain scalar reference; nothing here is
// wrong either, which is the point -- the refusal is a property of the PAIR.
static int helper(Ctx *c) { return c->a + c->b; }

// CABI-DAG: pub unsafe extern "C" fn __emitrust_cabi_inner(c: *mut Ctx) -> i32
int inner(Ctx *c) { return c->a * 2; }

// The delegating facade. Same signature as `inner`, refused.
// WARN: c-abi-exports-delegation-refused.c:[[#@LINE+2]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'outer': its parameter 'c' is a slice rather than a reference because the body forwards the whole pointer to 'helper', and forwarding keeps a reference only for an arithmetic pointee; a slice is a two-register fat pointer with no C spelling; it stays a plain 'pub fn' and is not reachable by dlsym
// WARN: c-abi-exports-delegation-refused.c:[[#@LINE+1]]:{{[0-9]+}}: note: the forwarding call is here; pass the helper what it actually needs, or give it a pointee the forwarding analysis tracks, and the parameter stays a reference
int outer(Ctx *c) { return helper(c); }

// CABI-DAG: pub unsafe extern "C" fn __emitrust_cabi_solo(c: *mut Ctx) -> i32
int solo(Ctx *c) { return c->b - 1; }

// THE REGRESSION GUARD, and it matters more than the new text. Genuinely
// unscalar signatures keep FR-139's sentence VERBATIM -- none of these is
// delegation-induced and none may pick up a word of the new wording.

struct hasptr {
  int n;
  int *p;
};

// WARN-DAG: c-abi-exports-delegation-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'by_value': it takes 'Hasptr' by value, whose layout model is not ABI-faithful (the pointer member 'p', emitted as an i64 data-pointer cursor rather than an address); it stays a plain 'pub fn' and is not reachable by dlsym
int by_value(struct hasptr h) { return h.n; }

// A REAL slice, demanded by a subscript in this very body: FR-138's shape,
// FR-138's unchanged sentence. Nothing delegates it, so no note is attached
// and none of the new words appear.
// WARN-DAG: c-abi-exports-delegation-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'walks': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int walks(int *p, int n) {
  int s = 0;
  int i;
  for (i = 0; i < n; ++i)
    s += p[i];
  return s;
}

// THE HONESTY CASE, and the reason the record carries a `soleReason` bit at
// all. `both` BOTH forwards its pointer AND subscripts it: the subscript is
// a slice demand of its own, so "it is a slice because you forward it" would
// be a LIE -- delete the call and the parameter is still a slice. A record
// that is not the sole reason is never surfaced, and this function keeps
// FR-139's sentence with no note attached.
// WARN-DAG: c-abi-exports-delegation-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'both': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int both(Ctx *c) { return helper(c) + c[1].a; }

// Not one word of the new wording may reach a function whose slice it does
// not explain, and no second note may appear anywhere in the stream.
// RUN: grep -c 'forwards the whole pointer' %t.cabi.err | FileCheck %s \
// RUN:   --check-prefix=ONEBLOCKER
// ONEBLOCKER: 1
// RUN: grep -c 'the forwarding call is here' %t.cabi.err | FileCheck %s \
// RUN:   --check-prefix=ONENOTE
// ONENOTE: 1
