// REQUIRES: cargo
// FR-50: `--search` may never produce a WORSE result than plain
// `--incremental`, and the two mechanisms that make that true.
//
// The bug this pins. FR-43's root state is FR-41's Green-or-Yellow set, and
// every child state admits STRICTLY FEWER items than its parent. So the search
// can only ever remove items -- never add one back. An item the coloring
// wrongly calls Red is therefore lost for good, with no diagnostic and no
// later stage that could recover it, and `--search` comes out BEHIND the
// greedy single import it was supposed to improve on. Measured on this file
// before the fix: 3 items ported with `--search`, 4 without.
//
// Two things had to change, and only both together give the guarantee.
//
// RUN: emitrust-cc --emit=search %s -o - | FileCheck %s --check-prefix=TRACE
//
// The baseline probe is not subject to the node budget. "Never worse than not
// searching" is a guarantee, and a guarantee cannot be switched off by a knob
// -- so a budget of one is RAISED to two exactly when the baseline differs
// from the root, and the trace says so in its own `max-nodes` token.
// RUN: emitrust-cc --emit=search --max-search-nodes=1 %s -o - \
// RUN:   | FileCheck %s --check-prefix=FLOOR
// FLOOR:      search items=6 roots=6 max-nodes=2
// FLOOR:      baseline 1 admitted=6 excluded=0
// FLOOR:      summary probes=2 {{.*}} >=baseline=yes
//
// The whole-project claim, stated as the thing a user actually cares about:
// the searched crate has at least as many real items as the unsearched one,
// and BOTH of them compile.
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.plain --build
// RUN: emitrust-cc --emit=crate --incremental --search %s -o %t.searched --build
// RUN: FileCheck %s --check-prefix=PLAIN --input-file=%t.plain/emitrust-progress.json
// RUN: FileCheck %s --check-prefix=SEARCHED --input-file=%t.searched/emitrust-progress.json

struct BaseA {
  int a;
};

// Genuinely unsupported: `CImporter::collectRecordFields` rejects a base class
// unconditionally, before it collects a single field.
struct Derived : BaseA {
  int b;
};

// (1) THE FALSE RED, fixed at the source. `Ref` reaches the Red `Derived` only
// through a POINTER, which the importer's pointer-struct-member models erase
// to an integer -- the emitted `struct Ref { d: i64, k: i32 }` never names
// `Derived` and compiles perfectly with `Derived` dropped. FR-41 used to call
// this Red through its `Field` edge and hand the search a smaller root state
// than the project deserved; the graph now records the edge as
// `FieldIndirect` and it carries no poison.
struct Ref {
  Derived *d;
  int k;
};

// (2) THE EXACT RED, confirmed rather than assumed. `Holder` embeds `Derived`
// BY VALUE, so its field list spells `Derived`. This one really is
// un-emittable, and the importer now agrees: since FR-50 it refuses to name a
// type whose own import it rejected, so `Holder` is rejected too instead of
// emitting a field of a struct nobody defines. That used to make plain
// `--incremental` report a FOURTH ported item and produce a crate that did
// not compile -- the "worse result" this test's premise was measured against
// was itself a fiction.
struct Holder {
  Derived d;
  int k;
};

int use(int x) { return x + 1; }

int main() { return use(1); }

// TRACE:      search items=6 roots=6 max-nodes=8

// `Ref` is admitted at the root now; only `Derived` and `Holder` are out.
// TRACE-NEXT: root 0 admitted=4 excluded=2
// TRACE-NEXT: probe 0 outcome=ok ported=4 stubbed=0 rep-cost=0 dropped=2

// (3) THE SAFETY NET. Node 1 is the BASELINE: the state admitting every item,
// whose probe is bit for bit the import a plain `--incremental` run performs.
// It is probed unconditionally, so `best` -- a maximum over the probed states
// -- can never fall below it, whatever FR-41 got wrong. Here it does not win,
// and in not winning it CONFIRMS the coloring: an import of all six items
// rejects exactly `Derived` and `Holder` and ports exactly the same four.
// TRACE: baseline 1 admitted=6 excluded=0
// TRACE-NEXT: probe 1 outcome=ok ported=4 stubbed=0 rep-cost=0 dropped=2
// TRACE-NEXT: learn 1 rejected=Derived as=drop tag=cxx-inheritance new=yes

// The cascade has its own tag: `Holder` is not a blocker, it names a type some
// OTHER item's rejection removed. FR-49's root-blocker table still credits
// `base-class`, which is the construct whose support would unblock both.
// TRACE-NEXT: learn 1 rejected=Holder as=drop tag=rejected-type-cascade new=yes

// TRACE: best 0 ported=4 stubbed=0 rep-cost=0
// TRACE: excluded Derived why=red
// TRACE-NEXT: excluded Holder why=red

// `>=baseline=yes` is the postcondition, printed on every run and asserted in
// `frontierSearch` before it returns.
// TRACE: summary probes=2 {{.*}} >=baseline=yes

// Four of six, both ways. The searched crate is not smaller than the
// unsearched one -- which is the entire claim -- and `--build` above proved
// both of them compile.
// PLAIN:      "ported": 4,
// SEARCHED:   "ported": 4,
