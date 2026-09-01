// FR-182: the EDGES of the two admitted struct shapes. Rejection is a feature
// here, and this file is the ledger of what stays rejected and WHY -- each
// residual class in its own words, at the C function's own location.
//
// Before FR-182 every refused signature got FR-139's single "not all-scalar"
// sentence, which is factually wrong for most of them: a struct-taking
// function's problem is not that a struct is unscalar, it is that the emitted
// Rust field list is not the C record's layout. Naming the ACTUAL divergence
// is the whole diagnostic value of the importer's faithfulness whitelist, so
// the exact wording is pinned, not the mere fact of a warning.
//
// The measured cases, in order:
//
// 1. A POINTER MEMBER in the pointee. `bitwriter_add` is the REQUIRED
//    NEGATIVE of this wave. The FR-181 spike byte-diffed it CLEAN, but only
//    because the function never reads `buffer` -- the importer keeps the
//    historical i64 CURSOR there, which is a data-pointer index and not an
//    address. That is a SEMANTIC lie, not a layout one: on LP64 it is the
//    same WIDTH as the pointer it replaces, so no size check and no offset
//    assertion can ever catch it. If this exports, the predicate is wrong.
//
// 2. A BIT-FIELD run, which the importer packs into a synthetic `__bitsN`
//    backing field. The importer's own comment calls that layout
//    "deliberately NOT ABI-compatible"; the emitted field list does not even
//    have the C member names in it.
//
// 3. A UNION anywhere, as a member here. A union maps either to a one-field
//    "slot" typed as a single arm (a reinterpretation of the others) or to an
//    opaque `[u8; N]` blob whose Rust alignment is 1 where the C union's is
//    its widest arm's. The refusal names the containing member AND the nested
//    reason, because "this struct is bad" is not actionable.
//
// 4. TWO reference parameters. This is THE structural cap and it is not
//    relaxable by analysis: two `&mut` are `noalias` to LLVM and a C caller
//    may legally alias them. MEASURED at rustc 1.96.1 -O3 --
//    `kernel(a,b){let t=b[0]; a[0]=99.0; b[1]=t+b[0];}` called with `a == b`
//    returned `b[1] == 10` where the clang native returned `104`, exit 0, no
//    diagnostic. No library can disprove that its caller aliases.
//
// 5. A SLICE parameter, which keeps FR-139's original wording verbatim: a
//    two-register fat pointer that shifts every later argument (measured on
//    crc16 -- native 21983, export 25322, exit 0, no diagnostic). Nothing
//    about that reasoning changed, so neither did the sentence.
//
// 6. An unfaithful struct BY VALUE, which is refused for its own reason and
//    says "takes ... by value" rather than "takes a pointer to".
//
// Everything here is a WARNING and not an error, for FR-139's reason: a crate
// holds many functions and typically only one of them is the dlsym target.
// But nothing may be silently exported wrong, so the file also pins that the
// crate gained NO C-ABI surface at all.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// Not one export, not one wrapper, not one `#[repr(C)]`: a struct is given
// the C layout promise only when something actually crosses the boundary with
// it, so a crate whose every candidate was refused is byte-identical to the
// flagless one.
// RUN: not grep no_mangle %t.cabi/src/lib.rs
// RUN: not grep export_name %t.cabi/src/lib.rs
// RUN: not grep 'extern "C"' %t.cabi/src/lib.rs
// RUN: not grep unsafe %t.cabi/src/lib.rs
// RUN: not grep 'repr(C)' %t.cabi/src/lib.rs
// RUN: not grep 'const _: ()' %t.cabi/src/lib.rs
//
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: diff %t.plain/src/lib.rs %t.cabi/src/lib.rs

typedef unsigned char tflac_u8;

// The corpus's bitwriter. `buffer` is the disqualifier and the ONLY one.
struct tflac_bitwriter {
  unsigned long long val;
  unsigned int bits;
  unsigned int pos;
  unsigned int len;
  unsigned int tot;
  tflac_u8 *buffer;
};

// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'bitwriter_add': it takes a pointer to 'TflacBitwriter', whose layout model is not ABI-faithful (the pointer member 'buffer', emitted as an i64 data-pointer cursor rather than an address); it stays a plain 'pub fn' and is not reachable by dlsym
int bitwriter_add(struct tflac_bitwriter *bw, unsigned int bits) {
  bw->bits += bits;
  bw->tot += bits;
  return (int)bw->tot;
}

struct flags {
  unsigned int lead : 3;
  unsigned int trail : 5;
  unsigned int count;
};

// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'flags_bump': it takes a pointer to 'Flags', whose layout model is not ABI-faithful (the bit-field run at member 'lead', packed into a synthetic __bits backing field whose layout is deliberately not ABI-compatible); it stays a plain 'pub fn' and is not reachable by dlsym
int flags_bump(struct flags *f, int d) {
  f->count += (unsigned)d;
  return (int)f->count;
}

union word { unsigned int u; float f; };
struct tagged { int tag; union word w; };

// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'tagged_tag': it takes a pointer to 'Tagged', whose layout model is not ABI-faithful (the member 'w', whose own type is not ABI-faithful (a C union, modeled as a single-arm slot or an opaque byte blob of alignment 1)); it stays a plain 'pub fn' and is not reachable by dlsym
int tagged_tag(struct tagged *t, int d) { return t->tag + d; }

// Both pointees are perfectly faithful. The cap is STRUCTURAL: it is the
// aliasing a C caller is allowed to do, not anything about the layout.
struct pt { int x; int y; };

// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'pt_mix': it takes 2 pointer arguments; a C caller may legally alias them, and two &mut references built from aliased pointers miscompile (measured); it stays a plain 'pub fn' and is not reachable by dlsym
int pt_mix(struct pt *a, struct pt *b) {
  a->x = b->y;
  return a->x;
}

// FR-138's shape, with FR-138's unchanged sentence.
// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'sum_bytes': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int sum_bytes(const unsigned char *p, int n) {
  int s = 0;
  int i;
  for (i = 0; i < n; ++i)
    s += p[i];
  return s;
}

struct hasptr { int n; int *p; };

// WARN-DAG: c-abi-exports-structs-refused.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'by_value_ptr': it takes 'Hasptr' by value, whose layout model is not ABI-faithful (the pointer member 'p', emitted as an i64 data-pointer cursor rather than an address); it stays a plain 'pub fn' and is not reachable by dlsym
int by_value_ptr(struct hasptr h) { return h.n; }
