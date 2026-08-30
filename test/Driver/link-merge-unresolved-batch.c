// FR-172: the link step reports EVERY unresolved external, not just the
// first one it trips over.
//
// The FR-58 obligation loop used to `return` the moment it found a symbol
// nobody on the link line defines, so a whole-program link with N missing
// symbols took N runs to enumerate them -- exclude one, re-link, learn the
// next. (The FR-166 spike burned a twelve-iteration exclude-and-retry loop
// on exactly this.) The FR-158 signature-divergence arm immediately above
// it already had the right shape: accumulate every located diagnostic, then
// fail once. This pins that the unresolved-external arm now matches it.
//
// This changes only HOW MANY missing symbols are reported. What a missing
// symbol DOES is unchanged: it is still a located hard error at the
// recorded declaration and the link still fails -- `link-merge-errors.c`
// pins that wording for the single-symbol case and must not move.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: rm -f %t.rs
// RUN: not emitrust-cc --link %t.main.o --emit=rust -o %t.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ALL
//
// All four names are reported from ONE run, each located at its own
// declaration, in declaration order.
// ALL: link-merge-unresolved-batch.c:[[#@LINE+9]]:5: error: unresolved external 'a1' at link
// ALL: link-merge-unresolved-batch.c:[[#@LINE+9]]:5: error: unresolved external 'a2' at link
// ALL: link-merge-unresolved-batch.c:[[#@LINE+9]]:5: error: unresolved external 'a3' at link
// ALL: link-merge-unresolved-batch.c:[[#@LINE+9]]:5: error: unresolved external 'a4' at link
//
// The merge still refuses to emit anything: a half-resolved module must
// never reach the splice.
// RUN: not ls %t.rs

int a1(int);
int a2(int);
int a3(int);
int a4(int);

int main(void) { return a1(1) + a2(2) + a3(3) + a4(4); }

// ...and exactly four, so a future change that double-reports, or one that
// silently regresses to reporting only the first, both fail here.
// RUN: not emitrust-cc --link %t.main.o --emit=rust -o %t.count.rs 2>&1 \
// RUN:   | grep -c "error: unresolved external" \
// RUN:   | FileCheck %s --check-prefix=COUNT
// COUNT: {{^}}4{{$}}
