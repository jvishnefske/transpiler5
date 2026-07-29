// REQUIRES: cargo
// FR-44 end-to-end claim: a project MOSTLY outside the supported subset still
// yields a cargo crate that BUILDS, and PORTING.md says exactly how much of it
// ported and what is holding the rest back.
//
// The out-of-subset part is drawn from the two blockers the RealWorld C++
// corpus's rejecting projects (`shapes`, `polygon`) are made of: a class with
// a base and a user-declared destructor, and a function taking a reference.
// Neither is stubbable as a whole item -- the class is dropped, the reference
// signature never maps -- so this exercises the case where the report's
// denominator can only come from the FR-40 item graph: `Base`, `Derived` and
// `bump` leave no trace whatsoever in the emitted module, and counting emitted
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

// --- Out of subset: a base class with a user-declared destructor, and a
// --- derived class. Both are DROPPED, taking their members with them.
class Base {
public:
  virtual ~Base();
  int seed;
};

class Derived : public Base {
public:
  int extra;
};

// --- Out of subset: a reference parameter. The signature never maps, so this
// --- is dropped rather than stubbed.
void bump(Counter &c) { c.n = c.n + 1; }

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
// The blocker table is the work queue, ranked; every C++ blocker appears.
// PORTING: ## Blockers, most items first
// PORTING-DAG: | cxx-destructor |
// PORTING-DAG: | cxx-inheritance |
// PORTING-DAG: | cxx-references |
//
// PORTING: ## Project items
// PORTING: | dropped | red | `Base` | record | cxx-destructor |
// PORTING: | dropped | red | `Derived` | record | cxx-inheritance |
// PORTING: | dropped | red | `bump` | function | cxx-references |
// PORTING: | ported | green | `Counter` | record |
// PORTING: | ported | green | `c_main` | function |
// PORTING: | ported | green | `total` | function |
// PORTING: | declared | grey | `printf` | function |

// STRICT: error: unsupported: user-declared destructor
