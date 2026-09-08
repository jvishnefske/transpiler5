// FR-202: the EDGES of the proven-bound slice export. Rejection is a feature,
// and every shape here is one where a bound is NOT provable -- so the export
// does not happen, the function keeps its plain `pub fn`, and it says so at its
// own location.
//
// This is the half of the increment that carries the safety argument. FR-181's
// measured failure mode for this exact feature is the worst one in the repo:
// a wrong slice length gives exit 0, no panic and ZERO diagnostics, invisible
// to everything except a byte-diff. An unsound bound would put that back. So
// each shape below is a reason a bound CANNOT be believed, and the intent is
// that if any of them ever exports, the analysis is wrong:
//
// 1. AN EARLY RETURN. `guarded` reads p[9] only when n != 0. A C caller may
//    legally pass a one-byte buffer with n == 0 and never touch p at all, so
//    "the maximum index in the body" is not a bound the caller owes. Building
//    a 10-byte slice from that pointer is instant UB before the body even
//    runs. The rule that catches it is structural: one basic block, no
//    region-carrying operation, so every access is on every path.
//
// 1b. A CONDITIONAL READ, which is the same hole wearing straight-line
//    clothes. `ternary` looks like one expression, but `c ? p[0] : p[9]` reads
//    ONE of them; so do `p[0] && p[9]` and `p[0] || p[9]`, whose short-circuit
//    C semantics are the same guarantee. All three import as an
//    `emitrust.if` with regions, so the SAME structural rule that catches the
//    early return catches them -- which is why the rule is "no region-carrying
//    operation" and not "no early return".
//
// 2. A CALL. `with_call` reads p[2] after calling a helper. A callee may
//    `exit()` or `longjmp` and never return, in which case the C program never
//    reads p[2] and the caller never owed those bytes. No call of any kind is
//    admitted -- and note this is NOT about the helper being unknown: the
//    helper here is a static function in this very file.
//
// 3. A RUNTIME INDEX. `sum_bytes` is FR-138's own shape: the extent is `n`, a
//    value, not a constant. This is where FR-181's "the integer after a
//    pointer is its length" rule died -- `synth_pair(pcm, nch, z)` has a
//    channel COUNT there whose real bound is `16*nch+1`, and `wcscat`'s
//    `numElem` is a CAPACITY its own corpus vector 5 has smaller than the
//    string. A loop bound is never a proof.
//
// 4. A WRITE, and with it every `&mut [T]`. `fill` is `unsigned char *`, which
//    the importer models as `&mut [u8]`. Two things are missing at once: this
//    wave proves READ extents only, and a `&mut` slice is `noalias` to LLVM
//    where a C caller may legally alias -- FR-181's HARD NO-GO, measured at
//    rustc 1.96.1 -O3 as native 104 against export 10, exit 0, no diagnostic.
//
// 5. A NON-BYTE ELEMENT. `word_sum` takes `const uint32_t *`. FR-182 measured
//    that an emitted Rust primitive is not always the width of the C type it
//    came from (`long double` -> `f64`, 128 bits narrowed to 64) and a bare
//    slice type carries no faithfulness verdict the way a record does. One
//    byte is the only width that is provably the C one. (The importer also
//    gives every non-const-byte pointer a `&mut` slice, so this shape is over
//    rule 4 as well; either refusal is correct.)
//
// 6. NO ACCESS AT ALL. `untouched` never reads p. The bound would be 0, and
//    `from_raw_parts(p, 0)` STILL requires p to be non-null and aligned --
//    while a C caller may legally pass NULL for a pointer nothing reads. A
//    zero bound is refused rather than exported.
//
// 7. A SLICE BESIDE A REFERENCE. `both` takes a byte slice and a struct
//    pointer. That is two references, and FR-181's structural cap is not
//    relaxable by any bound analysis: no library can disprove that its caller
//    aliased them.
//
// THE WORDING IS FR-139's, DELIBERATELY UNCHANGED. FR-182 kept that sentence
// verbatim for the slice class and it is pinned for `sum_bytes` in both
// test/Driver/c-abi-exports.c and test/Driver/c-abi-exports-structs-refused.c.
// Nothing about WHY these shapes are refused changed here -- they are still
// two-register fat pointers with no proven extent -- so neither does the
// sentence. What this file pins is that each shape is refused AT ITS OWN
// LOCATION and that the crate gains no C-ABI surface from any of them.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// Not one export, not one wrapper, not one `unsafe`: a crate whose every
// candidate was refused is byte-identical to the flagless one.
// RUN: not grep no_mangle %t.cabi/src/lib.rs
// RUN: not grep export_name %t.cabi/src/lib.rs
// RUN: not grep 'extern "C"' %t.cabi/src/lib.rs
// RUN: not grep unsafe %t.cabi/src/lib.rs
// RUN: not grep from_raw_parts %t.cabi/src/lib.rs
//
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: diff %t.plain/src/lib.rs %t.cabi/src/lib.rs

#include <stdint.h>

// 1. The early return: p[9] is not on every path.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'guarded': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int guarded(const unsigned char *p, int n) {
  if (n == 0)
    return 0;
  return p[0] + p[9];
}

// 1b. A conditional read: only one of p[0] and p[9] happens.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'ternary': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int ternary(const unsigned char *p, int c) { return c ? p[0] : p[9]; }

// The same guarantee through short-circuit `&&`: p[9] is read only when p[0]
// is non-zero.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'shortcircuit': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int shortcircuit(const unsigned char *p) { return p[0] && p[9]; }

static int helper(int x) { return x + 1; }

// 2. A call: the helper may never return, so p[2] is not owed.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'with_call': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int with_call(const unsigned char *p) { return helper(0) + p[2]; }

// 3. A runtime index: the extent is a value, not a constant.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'sum_bytes': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int sum_bytes(const unsigned char *p, int n) {
  int s = 0;
  int i;
  for (i = 0; i < n; ++i)
    s += p[i];
  return s;
}

// 4. A write, hence a `&mut [u8]`, hence the aliasing NO-GO too.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'fill': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
void fill(unsigned char *p) {
  p[0] = 1;
  p[1] = 2;
}

// 5. A non-byte element: the emitted primitive's width is not provably C's.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'word_sum': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
uint32_t word_sum(const uint32_t *p) { return p[0] + p[1]; }

// 6. No access at all: a zero-length slice still demands a non-null pointer.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'untouched': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int untouched(const unsigned char *p) {
  (void)p;
  return 7;
}

struct pt { int x; int y; };

// 7. A slice beside a struct pointer: two references, and the cap is
//    structural rather than analytical.
// WARN-DAG: c-abi-exports-slice-bound-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'both': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int both(const unsigned char *p, struct pt *q) { return p[0] + q->x; }
