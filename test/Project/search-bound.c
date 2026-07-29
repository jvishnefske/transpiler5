// FR-43: the search is BOUNDED, and the bound is counted in IMPORTS.
//
// `--max-search-nodes` limits how many candidate subsets are actually
// imported, not how many are generated, because the import is the only
// expensive thing in the loop -- a bound in states, edges or depth would not
// bound the wall clock at all. Two runs of the same project below, at
// `--max-search-nodes=1` and at the default, differ in exactly that: the first
// stops with `reason=node-budget` holding the root's failed candidate, the
// second spends a second import and finds a working one.
//
// The failing root is search-backtrack.cpp's shape in miniature: `absent` is
// declared and never defined, so the import survives every per-item decision
// and then dies whole-program in `finalizeProject`.
//
// The truncated run is not wrong, only unfinished: it reports the best
// candidate it managed to score, which is a failed one, and says so
// (`outcome=failed`, `improved=no`). A driver reading this must treat
// `reason=node-budget` as "there may be more" -- which is why the token is in
// the trace at all.
// RUN: emitrust-cc --emit=search --max-search-nodes=1 %s -o - \
// RUN:   | FileCheck --check-prefix=BOUND %s

int absent(int value);

int caller(int value) { return absent(value) + 1; }

int main(void) { return caller(1); }

// BOUND:      search items=3 roots=2 max-nodes=1
// BOUND-NEXT: root 0 admitted=3 excluded=0
// BOUND-NEXT: probe 0 outcome=failed ported=0 stubbed=0 rep-cost=0 dropped=0
// BOUND-NEXT: learn 0 failure=absent reason=unsupported: function 'absent' is referenced but not defined in any translation unit

// The children were GENERATED -- the budget stopped them being probed, not
// being proposed -- which is what `generated=3` against `probes=1` says.
// BOUND-NEXT: stop reason=node-budget
// BOUND-NEXT: best 0 ported=0 stubbed=0 rep-cost=0
// BOUND:      summary probes=1 generated=3 pruned=0 improved=no

// Zero is treated as one: a search that probes nothing has no candidate to
// return, and silently returning an unscored root would be worse than
// spending the single import that scores it.
// RUN: emitrust-cc --emit=search --max-search-nodes=0 %s -o - \
// RUN:   | FileCheck --check-prefix=ZERO %s
// ZERO:      search items=3 roots=2 max-nodes=1
// ZERO:      summary probes=1 {{.*}} improved=no

// With the default bound the same project finds its repair on the second
// import, well inside the eight it is allowed.
// RUN: emitrust-cc --emit=search %s -o - | FileCheck --check-prefix=FULL %s
// FULL:      search items=3 roots=2 max-nodes=8
// FULL:      child 1 from=0 drop=absent cascade=1 why=blamed
// FULL-NEXT: probe 1 outcome=ok ported=2 stubbed=1 rep-cost=0
// FULL:      stop reason=exhausted
// FULL-NEXT: best 1 ported=2 stubbed=1 rep-cost=0
// FULL:      summary probes={{[23]}} {{.*}} improved=yes
