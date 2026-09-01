// FR-182: the CONST arm of class 1. A struct pointer parameter reaches the
// emitter as `!emitrust.mut_ref<struct>` or `!emitrust.ref<struct>`, and the
// two are not interchangeable in the generated wrapper: `&mut *p` on a `&T`
// parameter is rustc E0596 and the whole crate is lost. The C importer
// currently produces the `mut_ref` form for every C pointer parameter, so the
// `ref` form has exactly one producer -- a C++ `const T&` -- and that is what
// this file exists to keep covered. Without it the `*const T` / `&*p` half of
// the wrapper is dead code that nothing would notice going wrong.
//
// The function is `extern "C"`, so it is a module-scope free function with a
// bare C symbol, exactly like the C cases in c-abi-exports-structs.c; a C++
// MEMBER stays refused (its receiver is synthesized and its symbol is not a
// name C ever had), which c-abi-exports-actor-cxx.cpp pins.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.cabi 2>%t.cabi.err
// RUN: not grep . %t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
//
// A const reference is a SHARED borrow: the wrapper must not fabricate a
// mutable one out of it.
// RUN: not grep '&mut' %t.cabi/src/lib.rs
// RUN: not grep '\*mut' %t.cabi/src/lib.rs
//
// RUN: emitrust-cc --emit=crate --crate-type=lib %s -o %t.plain 2>%t.plain.err
// RUN: not grep . %t.plain.err
// RUN: sed '/^#\[export_name/,$d' %t.cabi/src/lib.rs \
// RUN:   | grep -v '^#\[repr(C)\]$' \
// RUN:   | grep -v '^const _: () = assert!' > %t.cabi.stripped
// RUN: diff %t.plain/src/lib.rs %t.cabi.stripped

struct P {
  int x;
  int y;
};

extern "C" int psum(const P &p) { return p.x + p.y; }

// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct P {
// CABI-NEXT:     pub x: i32,
// CABI-NEXT:     pub y: i32,
// CABI-NEXT: }
// CABI-NEXT: const _: () = assert!(core::mem::size_of::<P>() == 8);
// CABI-NEXT: const _: () = assert!(core::mem::align_of::<P>() == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(P, x) == 0);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(P, y) == 4);
// CABI-NEXT: pub fn psum(p: &P) -> i32 {
// CABI:      #[export_name = "psum"]
// CABI-NEXT: pub unsafe extern "C" fn __emitrust_cabi_psum(p: *const P) -> i32 {
// CABI-NEXT:     psum(&*p)
// CABI-NEXT: }
