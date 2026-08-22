// FR-49: the progress report credits a cascaded rejection to the construct
// that actually caused it, not to the symptom the importer tripped over.
//
// The defect this exists to prevent is a blocker table that reads `other 13`
// on a project whose real backlog is two constructs. The importer's own
// diagnostic names a CONSEQUENCE (`method of an unimported class`, `excluded
// by the search state`), while the construct whose support would unblock the
// item is somewhere up the FR-41 poison chain. The report publishes both
// readings, ranks by the root one, and prints the chain so the attribution
// can be checked rather than trusted.
//
// The hard case is the off-graph one. C++ member functions are not FR-40
// item-graph nodes, so they have no color and no chain of their own; they
// are attributed through their enclosing CLASS, which has both.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate 2>/dev/null
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// The same input under FR-43's search, which rejects the POISONED items too
// and so produces the multi-hop chains a plain recovering import does not.
// RUN: emitrust-cc --emit=crate --incremental --search %s -o %t.search.crate \
// RUN:   2>/dev/null
// RUN: FileCheck %s --check-prefix=SEARCH \
// RUN:   --input-file=%t.search.crate/PORTING.md
//
// FR-49 is a REPORTING change: the emitted Rust is byte-identical to what a
// plain --recover run of the same input produces.
// RUN: emitrust-cc --emit=crate --recover %s -o %t.recover.crate 2>/dev/null
// RUN: diff %t.crate/src/main.rs %t.recover.crate/src/main.rs

// --- The first root: a user-declared destructor. `destructor` is written
// --- nowhere else in this file. W2.26 admitted the virtual destructor
// --- (sole-virtual-dtor classes are values now), so what keeps `Base` out
// --- is that the destructor has NO BODY in this translation unit.
class Base {
public:
  virtual ~Base();
  int seed;
};

// --- The second root: a base class. W2.26 admitted the droppy PUBLIC base
// --- (the old `cxx-drop-base` shape imports now), so the inheritance that
// --- keeps `Derived` out is PRIVATE -- still squarely the base-class root.
class Derived : Base {
public:
  int twice() const;
  int extra;
};

// --- An out-of-line member of a dropped class. Not a graph node, so its
// --- only route to a root is through `Derived`.
int Derived::twice() const { return extra * 2; }

// --- One hop further out: a class whose own declaration is entirely in
// --- subset and which is poisoned only by its field's type, plus a member
// --- of it that is therefore two hops from the cause.
class Holder {
public:
  int of() const;
  Derived member;
};

int Holder::of() const { return member.extra; }

// --- In subset, so the crate is not empty and the fraction is not zero.
int total(int a, int b) { return a + b; }

int main(void) { return total(20, 22); }

// The root table ranks by cause and names constructs, not diagnostics.
// PORTING: ## Root blockers, most items first
// PORTING: | root blocker | items |
// PORTING-DAG: | base-class |
// PORTING-DAG: | destructor |
//
// The direct table is the same items under the wording each one raised. Two
// of them are the content-free cascade wording, which before FR-49 was not
// classified at all and fell into the `other` bucket.
// PORTING: ## Direct blockers, as reported
// PORTING-DAG: | cxx-cascaded-method |
// W2.17, then W2.26, moved this pin FORWARD, never loosened it: W2.17
// admitted the non-virtual same-TU-defined destructor (the tag moved to
// `cxx-virtual-destructor`), and W2.26 admitted the sole-virtual-dtor
// class itself -- so what reports now is the destructor's missing BODY,
// under `cxx-destructor-no-body`. The ROOT attribution is unchanged
// through both moves -- `destructor` still ranks the item in the table
// above -- which is the property this test exists to pin.
// PORTING-DAG: | cxx-destructor-no-body |
// W2.18 moved this pin FORWARD too, and W2.26 again: a single public
// non-virtual base is admitted as a first field, and a droppy public base
// now imports as well (the old `cxx-drop-base` tag retired with the
// transitive drop predicate), so the inheritance that keeps `Derived` out
// is PRIVATE and reports the generic `cxx-inheritance` wording. The ROOT
// attribution is unchanged -- `base-class` still ranks the item in the
// table above -- which is the property this test exists to pin.
// PORTING-DAG: | cxx-inheritance |
//
// A graph item that IS its own root carries the one-element chain saying so.
// (The two rows are listed by direct blocker tag, and `cxx-destructor-no-body`
// sorts ahead of `cxx-inheritance`.)
// PORTING: ## Project items
// PORTING: | dropped | red | `Base` | record | destructor | Base | cxx-destructor-no-body |
// PORTING: | dropped | red | `Derived` | record | base-class | Derived | cxx-inheritance |
//
// The off-graph member function borrows its class's chain with itself
// prepended, so `twice` is credited to the base class it never mentions.
// PORTING: ## Rejected items outside the item graph
// PORTING: | dropped | red | `twice` | - | base-class | twice -> Derived | cxx-cascaded-method |

// The schema id is unchanged -- FR-49 only ADDS keys -- and `blockers` still
// carries the FR-44 meaning, the direct tally, with `root_blockers` beside
// it rather than in place of it.
// JSON: "schema": "emitrust-progress/1"
// JSON: "blockers": [
// JSON: "root_blockers": [
//
// `attributed_via` names the graph node an off-graph item borrowed its chain
// from, which is what makes the attribution auditable.
// JSON: "symbol": "twice"
// JSON: "root_blocker": "base-class"
// JSON-NEXT: "attributed_via": "Derived"
// JSON-NEXT: "blame_chain": ["twice", "Derived"]

// Under --search every poisoned item is rejected too, so the chains get
// longer and the point sharpens: `Holder` reports only that the search did
// not admit it, and `of` reports only that its class was not imported.
// NEITHER names a base class. Both are rooted at one, two and three symbols
// away respectively, and the chain shows the route.
// SEARCH: ## Root blockers, most items first
// SEARCH-DAG: | base-class |
// SEARCH-DAG: | destructor |
// SEARCH: ## Project items
// SEARCH: | dropped | red | `Holder` | record | base-class | Holder -> Derived | search-excluded |
// SEARCH: ## Rejected items outside the item graph
// SEARCH: | dropped | red | `of` | - | base-class | of -> Holder -> Derived | cxx-cascaded-method |
