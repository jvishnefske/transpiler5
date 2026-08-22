// REQUIRES: cargo
// FR-44 end-to-end claim: a project MOSTLY outside the supported subset still
// yields a cargo crate that BUILDS, and PORTING.md says exactly how much of it
// ported and what is holding the rest back.
//
// The out-of-subset part is drawn from the two blockers the RealWorld C++
// corpus's rejecting projects (`shapes`, `polygon`) are made of: a class with
// a base and a user-declared destructor, and a function returning a reference.
// Neither is stubbable as a whole item -- the class is dropped, the reference
// signature never maps -- so this exercises the case where the report's
// denominator can only come from the FR-40 item graph: `Base`, `Derived` and
// `pick` leave no trace whatsoever in the emitted module, and counting emitted
// symbols would silently report 3 of 3 instead of the honest 3 of 6.
//
// The crate is BUILT, not merely emitted: "it compiles" is the entire promise
// of --incremental, and a report over a crate that does not compile would be
// a fiction.
//
// RUN: emitrust-cc --emit=crate --incremental %s -o %t.crate --build 2>%t.err
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: %t.crate/target/release/incremental_builds; test $? -eq 42
//
// Without --incremental the same input yields nothing at all.
// RUN: not emitrust-cc --emit=crate %s -o %t.strict.crate 2>&1 \
// RUN:   | FileCheck %s --check-prefix=STRICT

extern "C" int printf(const char *, ...);

// --- In subset: a plain record and a free function over it.
struct Counter {
  int n;
};

int total(int a, int b) { return a + b; }

// --- Out of subset: a class whose destructor has no body in this TU, and
// --- a PRIVATELY derived class. Both are DROPPED, taking their members
// --- with them. (W2.26 admitted the virtual destructor itself and the
// --- droppy PUBLIC base, so the body-less dtor and the private
// --- inheritance are what keep these two out now.)
class Base {
public:
  virtual ~Base();
  int seed;
};

class Derived : Base {
public:
  int extra;
};

// --- Out of subset: a reference RETURN. FR-48 landed reference PARAMETERS
// --- (so `Counter &c` maps fine now, as `&mut Counter`), but returning a
// --- borrow would need a lifetime the model cannot derive, so the signature
// --- still never maps and this is dropped rather than stubbed -- keeping
// --- this file's third blocker `cxx-references`, as it was before FR-48.
int &pick(Counter &c) { return c.n; }

// `main` is in the subset on its own and stays a real, running function: the
// crate's binary exits 42, which is what makes "the crate builds" a claim
// about working code rather than about a pile of stubs.
int main(void) {
  Counter c;
  c.n = total(20, 22);
  printf("n=%d\n", c.n);
  return c.n;
}

// PORTING: # Porting status: `incremental_builds`
// PORTING: **3 of 6 items ported (50.0%).**
// PORTING: | ported | 3 |
// PORTING-NEXT: | stubbed | 0 |
// PORTING-NEXT: | dropped | 3 |
// PORTING-NEXT: | missing | 0 |
//
// The `printf` prototype is `declared`: the project never defines it, so
// there is nothing to port and it is excluded from the fraction -- 6 portable
// items out of 7 rows.
// PORTING-NEXT: | declared | 1 |
//
// The blocker tables are the work queue, ranked. FR-49 puts the ROOT table
// first: here every rejected item is its OWN root (each names a construct
// the FR-41 probe rejects directly, and none of the three depends on
// another), so the two tables agree item for item -- but they say so in
// their own vocabularies, the probe's construct names above and the FR-42
// diagnostic tags below. Neither is translated into the other.
// PORTING: ## Root blockers, most items first
// PORTING-DAG: | base-class |
// PORTING-DAG: | destructor |
// PORTING-DAG: | reference-type |
//
// PORTING: ## Direct blockers, as reported
// W2.17, then W2.26, moved the `Base` pin FORWARD, never loosened it:
// W2.17 admitted the non-virtual same-TU-defined destructor (the tag
// moved to `cxx-virtual-destructor`), and W2.26 admitted the
// sole-virtual-dtor class itself, so what reports now is the destructor's
// missing BODY, under `cxx-destructor-no-body` -- and the item rows,
// which are ordered by that tag, reorder with it. The ROOT attribution is
// unchanged (`destructor` still ranks the item in the table above), which
// is the property this test exists to pin.
//
// W2.18 and then W2.26 moved the `Derived` row forward the same way: a
// single public non-virtual base is admitted as a first field, and a
// droppy PUBLIC base now imports too (the old `cxx-drop-base` tag retired
// with the transitive drop predicate), so what keeps `Derived` out is its
// PRIVATE inheritance, under the generic `cxx-inheritance` wording. The
// root attribution is still `base-class`.
// PORTING-DAG: | cxx-destructor-no-body |
// PORTING-DAG: | cxx-inheritance |
// PORTING-DAG: | cxx-references |
//
// PORTING: ## Project items
// PORTING: | dropped | red | `Base` | record | destructor | Base | cxx-destructor-no-body |
// PORTING: | dropped | red | `Derived` | record | base-class | Derived | cxx-inheritance |
// PORTING: | dropped | red | `pick` | function | reference-type | pick | cxx-references |
// PORTING: | ported | green | `Counter` | record |
// PORTING: | ported | green | `c_main` | function |
// PORTING: | ported | green | `total` | function |
// PORTING: | declared | grey | `printf` | function |

// STRICT: error: unsupported: destructor with no definition in this translation unit
