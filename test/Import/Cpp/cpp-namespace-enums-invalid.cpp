// The RESIDUAL frontier left after the enum symbol learned its namespace
// prefix, and the wording fix that came with it.
//
// The pre-existing enum dedup diagnostic said "with a different shape in
// another translation unit" UNCONDITIONALLY. That clause was factually
// false for every collision inside ONE translation unit, and because the
// key was the bare C tag, a single-TU collision was the COMMON case: two
// enums in different namespaces produced it. The prefix removes the false
// positives; this file pins what is left, and pins that each surviving
// rejection now says which of the two situations it is.
//
// Rejection is the feature here. None of these three is silently merged
// into one Rust type, and none of them is a program the importer may
// guess at:
//   * SAME TU, record-nested vs file-scope. `namespacePrefix` walks
//     NamespaceDecls only, so a `RecordDecl` owner contributes nothing --
//     the same posture `recordRustName` takes for a nested class. Both
//     enums emit `E`, and the shapes differ, so it is a located refusal
//     with the SAME-TU wording, not the cross-TU one.
//   * SAME TU, block scope vs file scope. Records take a
//     `<function>_<tag>` block-scope mangle in `importRecord`; enums have
//     no such mangle, so a function-local `enum E` composes onto the
//     file-scope one. Refused located, again with the same-TU wording.
//   * SAME TU, FR-125 case fold. `namespace Game` and `namespace game`
//     both fold to `ns_game_` under the idiomatic rename, so `Game::E`
//     and `game::E` compose ONE symbol. This is the enum flavor of the
//     record guard pinned in cpp-ns-case-fold-invalid.cpp, and it is
//     mode-sensitive by construction: with the rename OFF the segments
//     keep their case, the symbols differ, and both enums import (the
//     PRESERVED leg is that additivity control).
//
// The CROSSTU leg is the control in the other direction: a genuine
// two-TU shape conflict -- the behavior the dedup check exists for --
// still rejects, and keeps the "in another translation unit" wording,
// which is true there.
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=rust %t/nested.cpp 2>&1 | FileCheck %s --check-prefix=NESTED
// RUN: not emitrust-cc --emit=rust %t/block.cpp 2>&1 | FileCheck %s --check-prefix=BLOCK
// RUN: not emitrust-cc --emit=rust %t/casefold.cpp 2>&1 | FileCheck %s --check-prefix=CASEFOLD
// RUN: emitrust-import-c %t/casefold.cpp | FileCheck %s --check-prefix=PRESERVED
// RUN: emitrust-cc --recover --emit=rust %t/casefold.cpp 2>&1 >/dev/null | FileCheck %s --check-prefix=RECDIAG
// RUN: emitrust-cc --recover --emit=rust %t/casefold.cpp | FileCheck %s --check-prefix=RECOVER --implicit-check-not="NsGameE(2)"
// RUN: not emitrust-import-c %t/tu1.cpp %t/tu2.cpp 2>&1 | FileCheck %s --check-prefix=CROSSTU

//--- nested.cpp
struct S { enum E { A = 3, B = 4 }; int v; };
enum E { X = 7, Y = 8 };
int main() { S s; s.v = (int)S::A; return s.v + (int)X; }

// NESTED: nested.cpp:1:12: error: unsupported: enum 'E' collides with the emitted name of a different enum in this translation unit

//--- block.cpp
enum E { A = 1 };
int f() { enum E { A = 2 }; return (int)A; }
int main() { return (int)A + f(); }

// BLOCK: block.cpp:2:11: error: unsupported: enum 'E' collides with the emitted name of a different enum in this translation unit

//--- casefold.cpp
namespace Game { enum E { A = 1 }; }
namespace game { enum E { A = 2 }; }
int main() { return (int)Game::A + (int)game::A; }

// CASEFOLD: casefold.cpp:2:18: error: unsupported: enum 'NsGameE' collides with the emitted name of a different enum in this translation unit

// With the rename off nothing folds together: both namespaces keep their
// verbatim segment and BOTH enums import.
// PRESERVED-DAG: emitrust.enum_def @ns_Game_E ["A"] [1]
// PRESERVED-DAG: emitrust.enum_def @ns_game_E ["A"] [2]

// The FR-42 recovery half, which every located rejection owes (recovery
// structurally manufactures dead functions, so any rule that must hold
// for emitted code has to survive `--recover`): the SECOND enum is
// DROPPED -- never merged into the first -- the function that named it
// takes a loud `unimplemented!()` stub, and the item is tabulated under
// its OWN ledger tag rather than the catch-all `other`. This leg is what
// pins lib/ImportC/RejectionLedger.cpp's needle, whose hand-mirrored twin
// in test/RealWorld/run_realworld.py's `classify_blocker` must agree.
// RECDIAG: casefold.cpp:2:18: warning: unsupported: enum 'NsGameE' collides with the emitted name of a different enum in this translation unit (recovered: item dropped)
// RECDIAG: dropped 'NsGameE' [enum-name-clash] unsupported: enum 'NsGameE' collides with the emitted name of a different enum in this translation unit
// RECDIAG: stubbed 'c_main' [rejected-type-cascade]
// RECDIAG: enum-name-clash 1

// The recovered crate keeps the FIRST enum, which legitimately owns the
// name; `game::E::A`'s value 2 must not reach emitted Rust under any
// name, which is what the whole-crate `--implicit-check-not` scans for.
// RECOVER: const A: NsGameE = NsGameE(1);
// RECOVER: unimplemented!(

//--- tu1.cpp
namespace lib { enum Color { Red = 1, Green = 2 }; }
int t1() { return (int)lib::Red; }

//--- tu2.cpp
namespace lib { enum Color { Red = 1, Green = 3 }; }
int t2() { return (int)lib::Green; }

// The cross-TU wording is retained where it is TRUE, and it now names the
// emitted symbol, which is the key the dedup actually collided on.
// CROSSTU: tu2.cpp:1:17: error: unsupported: conflicting definition of enum 'ns_lib_Color' with a different shape in another translation unit
