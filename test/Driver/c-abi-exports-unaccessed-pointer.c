// FR-226: a pointer parameter the body NEVER ACCESSES gets a C-ABI export --
// with no bound, no `from_raw_parts`, and no `unsafe` anywhere in the crate.
//
// WHAT THIS PINS AND WHY IT EXISTS. The TRACTOR corpus's
// `SPX_initialize_hash_function` is, in its blake configurations, literally
// `void initialize_hash_function(spx_ctx *ctx) { (void)ctx; }`. It imports
// FAITHFULLY as `pub fn spx_initialize_hash_function(_ctx: &mut [u8]) {}` and
// then `--c-abi-exports` emitted NOTHING for it, so the harness's `dlsym`
// found no symbol and 12 corpus cases scored SYMBOL_MISSING. The function that
// is trivially the SAFEST thing in the crate to export was the one class that
// could not be exported, and the reason is an accident of how the two existing
// pointer classes are proven:
//
//   * FR-182 class 1 wants a struct REFERENCE, and a C `T *` parameter is
//     imported as `&mut [T]` -- a SLICE -- essentially always;
//   * FR-202 class 2 wants a MUST-ACCESS BOUND proven from the body, and this
//     body has zero accesses, so there is nothing to prove a bound from.
//
// THE CLAUSE THIS OVERTURNS, and why the overturning is narrow.
// `cAbiProvenSliceBound` refuses a zero bound, with this reasoning:
// "`from_raw_parts(p, 0)` still requires `p` to be non-null and aligned, while
// a C caller may legally pass NULL for a pointer nothing reads." Every word of
// that is true and the conclusion does not follow. It is an argument against
// BUILDING A SLICE FROM THE POINTER, not against exporting the function. This
// class builds nothing: the wrapper takes the raw pointer, IGNORES it, and
// hands the translated function an EMPTY slice of its own. `from_raw_parts` is
// never called, so its precondition never arises, and the export is therefore
// sound for ANY pointer value -- null, dangling, misaligned, an integer the
// caller invented. That is exactly the case the old comment worried about.
//
// WHAT MAKES IT SOUND, stated as the proof obligation the analysis discharges:
// every use of the parameter in the whole function body is an `emitrust.deref`
// whose result has NO USERS. That is a universally quantified negative over
// the SSA use-list, which is complete by construction -- a value is observable
// only through an operand, and a use in a nested region is still a use -- so
// unlike the must-access proof it needs no single-block rule and no operation
// whitelist. `(void)ctx;` is precisely a deref with no users; it emits no Rust
// at all, which is why the translated body is `{}`.
//
// MUTABILITY IS IRRELEVANT, and this is the one line a reviewer should
// challenge. FR-181's HARD NO-GO is that two `&mut` references built from
// pointers a C caller legally aliased are `noalias` to LLVM and miscompile
// (native 104 against export 10, exit 0, no diagnostic). Nothing here is built
// from the caller's pointer: `&mut []` is a fresh zero-length temporary of the
// wrapper's own, borrowing none of the caller's storage, so there is no memory
// for an aliasing promise to be wrong about. The structural cap is untouched
// regardless -- this class admits ONE reference parameter, like every other,
// and `two_noop` below pins that two are still refused.
//
// NO `unsafe`, AND THAT IS A PROPERTY OF THE CLASS. Every other wrapper
// reconstitutes a Rust reference from a raw pointer and owes the validity
// obligation the keyword marks. This one performs no unsafe operation, so it
// is a plain safe `extern "C" fn` -- the identical symbol, the identical ABI,
// and the rubric's zero-`unsafe` score kept.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.cabi.err
//
// Four exports, four wrappers, and NOT ONE `unsafe`, `from_raw_parts` or
// `no_mangle`: nothing here is all-scalar, so the C symbol can only come from
// a wrapper, and this class's wrapper is safe Rust.
// RUN: grep -c export_name %t.cabi/src/lib.rs > %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count
// RUN: not grep unsafe %t.cabi/src/lib.rs
// RUN: not grep from_raw_parts %t.cabi/src/lib.rs
// RUN: not grep no_mangle %t.cabi/src/lib.rs
//
// The pointee is spelled OPAQUE. The wrapper never looks behind the pointer,
// so it asserts no layout: naming `SpxCtx` would demand the caller's object
// match a Rust layout model and would drag the struct into the `#[repr(C)]`
// closure for nothing. `c_void` is ABI-identical to any other thin pointer.
// RUN: not grep 'repr(C)' %t.cabi/src/lib.rs
// RUN: not grep 'offset_of' %t.cabi/src/lib.rs
//
// The byte-identity guard: the flag is default OFF, and the wrapper epilogue
// is a pure module-scope APPEND -- deleting it reproduces the flagless crate
// root byte for byte, which is also how the "no repr(C) added" claim above is
// checked against the whole file rather than one grep.
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: not grep 'extern "C"' %t.plain/src/lib.rs
// RUN: sed '/^#\[export_name/,$d' %t.cabi/src/lib.rs > %t.cabi.stripped
// RUN: diff %t.plain/src/lib.rs %t.cabi.stripped

typedef struct {
  unsigned char seed[32];
  int n;
} spx_ctx;

// THE CORPUS SHAPE, verbatim from the blake configurations of
// `SPX_initialize_hash_function`. The parameter is a struct pointer, so the
// importer models it `&mut [SpxCtx]` -- a MUTABLE slice of a NON-byte element,
// both of which class 2 refuses and neither of which matters when nothing is
// accessed.
// CABI: pub fn initialize_hash_function(_ctx: &mut [SpxCtx]) {
void initialize_hash_function(spx_ctx *ctx) { (void)ctx; }

// A SHARED slice, from a `const unsigned char *`. This exact function was
// pinned as REFUSED by c-abi-exports-slice-bound-refused.c's reason 6 until
// FR-226; the pin moved forward and lives here now.
// CABI: pub fn untouched(_p: &[u8]) -> i32 {
int untouched(const unsigned char *p) {
  (void)p;
  return 7;
}

// A SCALAR AFTER the pointer. This is FR-181's measured shift: had the slice
// been exported directly, `k` would receive the fat pointer's length instead
// of the caller's integer. The wrapper's pointer is ONE register.
// CABI: pub fn trail(_p: &mut [u8], k: i32) -> i32 {
int trail(unsigned char *p, int k) {
  (void)p;
  return k + 11;
}

// A SCALAR BEFORE it: the reference is not required to be parameter zero.
// CABI: pub fn lead(k: i32, _p: &mut [u8]) -> i32 {
int lead(int k, unsigned char *p) {
  (void)p;
  return k * 3;
}

// ---------------------------------------------------------------------------
// THE REFUSALS. Each keeps FR-139's sentence VERBATIM -- nothing about why
// these shapes have no C spelling changed, so neither does the wording -- and
// each lands at ITS OWN location.
// ---------------------------------------------------------------------------

// THE SYMMETRY PIN, and the most important line in this file. `write_bytes`
// has the SAME SIGNATURE as `trail`'s pointer and differs by exactly one
// access. If a later refactor ever widens the class, this is what catches it:
// one added store and the function must stop qualifying.
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'write_bytes': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
void write_bytes(unsigned char *p) { p[0] = 1; }

// The READ half of the same symmetry: `read_byte` differs from `untouched` by
// one load. (It has a provable bound of 1, so it is class 2's business, not
// this class's -- and either way it is not an unaccessed pointer. What is
// pinned here is that the mutable-slice shape below it does NOT become
// exportable just because the read is the only access.)
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'read_byte_mut': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int read_byte_mut(unsigned char *p) { return p[0]; }

// TWO unaccessed pointers. Whether two never-accessed pointers are also safe
// is a real question and this wave does not answer it: the structural cap is
// left exactly where FR-181 put it, and the second reference is refused
// without being analysed at all.
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'two_noop': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
void two_noop(unsigned char *a, unsigned char *b) {
  (void)a;
  (void)b;
}

// ACCESSED ON ONE PATH ONLY. `maybe` reads p[0] when n != 0 and never
// otherwise, so on one path it looks exactly like an unaccessed pointer. It is
// refused, and the reason the analysis does not have to reason about paths at
// all is that the read is a USE of the parameter's deref no matter which
// region it sits in.
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'maybe': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int maybe(unsigned char *p, int n) {
  if (n)
    return p[0];
  return 0;
}

static void store_one(unsigned char *q) { q[0] = 2; }

// THE POINTER ESCAPES WITHOUT THIS BODY TOUCHING IT. `forward` never
// subscripts `p` itself; it hands the whole thing to a helper that does. The
// deref therefore HAS a user -- a `slice_of` that reborrows it into the call --
// and that is why the rule is "a deref with no users" rather than "no
// subscript in this body". An empty slice here would silently truncate the
// helper's view of the caller's buffer.
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'forward': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
void forward(unsigned char *p) { store_one(p); }

struct pt {
  int x;
  int y;
};

// A SECOND POINTER that is also never accessed. Same answer as `two_noop`, and
// pinned separately because the two arrive at the refusal by different counts
// (two slices here as well -- a C `struct pt *` is imported as a slice too).
// WARN-DAG: c-abi-exports-unaccessed-pointer.c:[[#@LINE+1]]:{{[0-9]+}}: warning: --c-abi-exports: no C-ABI export for 'mixed': its signature is not all-scalar (a C-ABI entry point may only take and return builtin integer and floating-point types); it stays a plain 'pub fn' and is not reachable by dlsym
int mixed(const unsigned char *p, struct pt *q) {
  (void)p;
  (void)q;
  return 1;
}

// The wrapper epilogue. Each item is named for the bare C symbol with the
// `__emitrust_cabi_` prefix (reserved in C, so it cannot collide with an
// imported name) and binds the bare symbol through `#[export_name]`, leaving
// the translated function and every internal call site byte for byte
// unchanged. Note the missing `unsafe` and the `c_void` pointee.
// CABI:      #[export_name = "initialize_hash_function"]
// CABI-NEXT: pub extern "C" fn __emitrust_cabi_initialize_hash_function(_ctx: *mut core::ffi::c_void) {
// CABI-NEXT:     initialize_hash_function(&mut [])
// CABI-NEXT: }
// CABI:      #[export_name = "untouched"]
// CABI-NEXT: pub extern "C" fn __emitrust_cabi_untouched(_p: *const core::ffi::c_void) -> i32 {
// CABI-NEXT:     untouched(&[])
// CABI-NEXT: }
// CABI:      #[export_name = "trail"]
// CABI-NEXT: pub extern "C" fn __emitrust_cabi_trail(_p: *mut core::ffi::c_void, k: i32) -> i32 {
// CABI-NEXT:     trail(&mut [], k)
// CABI-NEXT: }
// CABI:      #[export_name = "lead"]
// CABI-NEXT: pub extern "C" fn __emitrust_cabi_lead(k: i32, _p: *mut core::ffi::c_void) -> i32 {
// CABI-NEXT:     lead(k, &mut [])
// CABI-NEXT: }

// COUNT: 4
