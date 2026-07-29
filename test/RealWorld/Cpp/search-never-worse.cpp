// FR-50 anti-regression gate: on every project of the FR-46 C++ corpus,
// `--emit=crate --incremental --search` must port AT LEAST as many items as
// the same command without `--search`.
//
// RUN: %python %S/../search_never_worse.py --emitrust-cc emitrust-cc \
// RUN:   --corpus %S/Inputs --workdir %t | FileCheck %s
//
// This file has a .cpp suffix only so lit discovers it (test/lit.cfg.py sets
// config.suffixes = [".mlir", ".c", ".cpp"]); it contains no C++ code. The
// corpus projects live under Inputs/, which config.excludes keeps out of
// discovery, and are driven through their checked-in compile_commands.json by
// ../search_never_worse.py.
//
// WHY A SEPARATE GATE, next to the RealWorld C++ one that already transpiles
// these same projects. Two reasons, and both are about what the gate is
// checking rather than about what it runs:
//
//  1. It is the only test in the tree whose reference is NOT the coloring.
//     Every FileCheck over `--emit=coloring` or `--emit=search` asserts that
//     the analysis says what it currently says; if FR-41 starts calling a
//     portable item Red, those tests are updated to match and the loss is
//     invisible. Here the reference is the unrestricted recovering import --
//     a fact about the IMPORTER -- so the two sides can disagree, and the
//     disagreement is the failure.
//  2. The failure it guards against is silent and irreversible. FR-43's root
//     state is the Green-or-Yellow set and every child admits strictly fewer
//     items than its parent, so the search can only ever REMOVE. An item the
//     coloring wrongly excludes is gone from the crate with no diagnostic, no
//     `excluded` line a person would question, and no later stage that could
//     recover it.
//
// Unlike realworld-cpp.cpp this needs no `cargo`: nothing is built, only
// transpiled and counted, so the gate runs on every configuration.
//
// The MEASURED numbers as of FR-50 (`fixed-stats` and `tokenizer` are wholly
// inside the subset; `polygon` and `shapes` are mostly outside it and are the
// interesting cases). Both sides are checked, not just the verdict, so a
// change that lowers BOTH counts equally still shows up in the diff.
//
// CHECK: fixed-stats   no-search 11/11   --search 11/11   ok
// Ratcheted 2/8 -> 4/8 by FR-48 (C++ reference parameters), which landed
// after this gate was written. The INVARIANT this file exists to protect is
// the trailing `ok` on every row and the summary line below; the fractions
// are pinned alongside it so a silent DROP is a failure too, and they move
// only forward.
// CHECK: polygon       no-search 4/8   --search 4/8   ok
// CHECK: shapes        no-search 1/7   --search 1/7   ok
// CHECK: tokenizer     no-search 5/5   --search 5/5   ok
// CHECK: All 4 corpus projects: --search >= no-search.
