// FR-182 REGRESSION, found by probe and fixed in the same change: the C-ABI
// faithfulness verdict is memoized per RecordDecl, and a record's members are
// answered from that memo -- so a member's record type must have an entry
// before the containing record is judged.
//
// Every TU has its OWN decl for a record reached through a shared header, and
// the cross-TU dedup merges them by EMITTED NAME: the second TU's record is
// recognized as the same shape and skipped BEFORE any struct_def is created.
// The verdict was written only where a struct_def was built, so the second
// TU's decl had no entry at all -- and a record of THAT TU containing the
// shared one was refused with "whose record type carries no imported layout
// model". Safe direction (a refusal, never a wrong export), but a measured
// loss on exactly the multi-TU shape the corpus is made of.
//
// The fix records the verdict on the dedup path too. It is the SAME verdict
// by construction: the dedup has just proven the two field-name/field-type
// arrays identical, which is what the verdict is computed from.
//
// This file is the SECOND translation unit on the command line, so its
// `struct inner` decl is the merged one and `struct wrap` is the record whose
// member needs the memo.
//
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports \
// RUN:   %S/Inputs/c-abi-exports-structs-multi-tu-lib.c %s -o %t.cabi \
// RUN:   2>%t.cabi.err
// RUN: cat %t.cabi/src/lib.rs | FileCheck %s --check-prefix=CABI
//
// Not one refusal: the driver's per-file progress lines are all that reaches
// stderr, and neither export is reported blocked.
// RUN: not grep 'no C-ABI export' %t.cabi.err
//
// ONE `Inner` in the whole crate (the merge still happens), all three records
// carry the layout promise, and both entries are reachable by dlsym.
// RUN: grep -c 'pub struct Inner {' %t.cabi/src/lib.rs > %t.cabi.count
// RUN: grep -c 'repr(C)' %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: grep -c export_name %t.cabi/src/lib.rs >> %t.cabi.count
// RUN: FileCheck %s --check-prefix=COUNT --input-file=%t.cabi.count

#include "Inputs/c-abi-exports-structs-multi-tu-shared.h"

// A record of THIS translation unit whose member is the merged one.
struct wrap {
  struct inner in;
  int k;
};

int wrap_sum(struct wrap *w) { return w->in.a + w->in.b + w->k; }

// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct Inner {
// CABI:      const _: () = assert!(core::mem::size_of::<Inner>() == 8);
// CABI:      #[repr(C)]
// CABI-NEXT: #[derive(Clone, Copy, Default)]
// CABI-NEXT: pub struct Wrap {
// CABI-NEXT:     pub in_: Inner,
// CABI-NEXT:     pub k: i32,
// CABI-NEXT: }
// CABI-NEXT: const _: () = assert!(core::mem::size_of::<Wrap>() == 12);
// CABI-NEXT: const _: () = assert!(core::mem::align_of::<Wrap>() == 4);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Wrap, in_) == 0);
// CABI-NEXT: const _: () = assert!(core::mem::offset_of!(Wrap, k) == 8);
// CABI:      #[export_name = "inner_sum"]
// CABI:      #[export_name = "wrap_sum"]

// COUNT:      1
// COUNT-NEXT: 2
// COUNT-NEXT: 2
