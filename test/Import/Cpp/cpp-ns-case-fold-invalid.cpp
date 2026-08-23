// FR-125 located-rejection ledger for CASE-FOLD collisions: the idiomatic
// rename folds namespace segments (and free-function base names) to
// snake_case, so two DIFFERENT qualified C++ spellings can compose to ONE
// emitted symbol -- `Game::f` and `game::f` both emit `ns_game_f`,
// `n::myFunc` lands on `n::my_func`'s spelling. The fold must never merge
// them; every collision rejects LOCATED at the later declaration.
//
// The dangerous shape is PROTO-FUSION: the symbol-reconciliation's
// redundant-declaration early-success cannot tell a redeclaration from a
// case-fold alias, so before the qualified-owner guard a prototype-only
// `n::myFunc` was silently "satisfied" by `n::my_func`'s definition --
// measured at HEAD pre-fix: `n::myFunc(1)` compiled into a call to
// `ns_n_my_func`, exit 0, wrong body where the native build link-fails.
// The FR-73 guard is blind here (both raw spellings are `f`, no leading
// underscore), so the guard is keyed on the QUALIFIED spelling instead.
//
// Like FR-108's record guard, the new guard is keyed on the EMITTED name
// and therefore mode-sensitive by construction: with the rename off the
// spellings never collide, and `emitrust-import-c` keeps transpiling
// both (the PRESERVED leg). The record flavor (`Game::Box` beside
// `game::Box`) already collided pre-FR-125 -- the UpperCamel fold erased
// the case first -- and keeps FR-108's own wording (the RECORD leg).
// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=rust %t/defs-collide.cpp 2>&1 | FileCheck %s --check-prefix=DEFS
// RUN: not emitrust-cc --emit=rust %t/proto-fusion.cpp 2>&1 | FileCheck %s --check-prefix=PROTO
// RUN: not emitrust-cc --emit=rust %t/member-fold.cpp 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-cc --emit=rust %t/record-fold.cpp 2>&1 | FileCheck %s --check-prefix=RECORD
// RUN: emitrust-import-c %t/defs-collide.cpp | FileCheck %s --check-prefix=PRESERVED

//--- defs-collide.cpp
namespace Game { int f(int x) { return x + 1; } }
namespace game { int f(int x) { return x + 2; } }
int main() { return Game::f(1) + game::f(2); }

// Two definitions distinguished only by the namespace's case: located at
// the later one, both qualified spellings named.
// DEFS: defs-collide.cpp:2:22: error: unsupported: function 'game::f' emits as 'ns_game_f', which collides with 'Game::f' (the idiomatic rename folds both spellings onto one symbol)

// With the rename off nothing collides: both namespaces keep their
// verbatim segment and both functions import.
// PRESERVED-DAG: func.func @ns_Game_f(
// PRESERVED-DAG: func.func @ns_game_f(

//--- proto-fusion.cpp
namespace Game { int f(int x); }
namespace game { int f(int x) { return x + 2; } }
int main() { return Game::f(1); }

// The prototype-only spelling must NOT be satisfied by the case-fold
// alias's body: rejected located, never a bound call.
// PROTO: proto-fusion.cpp:2:22: error: unsupported: function 'game::f' emits as 'ns_game_f', which collides with 'Game::f' (the idiomatic rename folds both spellings onto one symbol)

//--- member-fold.cpp
namespace n { int myFunc(int x); }
namespace n { int my_func(int x) { return x + 2; } }
int main() { return n::myFunc(1); }

// The same guard closes the pre-existing BASE-NAME fold fusion
// (`mangleMemberName`'s snake_case): the measured silent-miscompile
// shape above.
// MEMBER: member-fold.cpp:2:19: error: unsupported: function 'n::my_func' emits as 'ns_n_my_func', which collides with 'n::myFunc' (the idiomatic rename folds both spellings onto one symbol)

//--- record-fold.cpp
namespace Game { struct Box { int v; int get() const { return v; } }; }
namespace game { struct Box { int w; int get() const { return w + 1; } }; }
int main() { Game::Box a{1}; game::Box b{2}; return a.get() + b.get(); }

// Records case-folded before FR-125 (UpperCamel), and FR-108's guard
// already rejects the fusion with its own wording -- regression-pinned
// so the namespace-segment fold cannot reopen the channel.
// RECORD: record-fold.cpp:2:18: error: unsupported: struct 'NsGameBox' collides with the emitted name of a different struct in this translation unit
