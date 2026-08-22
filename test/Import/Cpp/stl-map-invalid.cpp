// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/unordered-map.cpp 2>&1 | FileCheck %s --check-prefix=UNORDMAP
// RUN: not emitrust-import-c %t/unordered-set.cpp 2>&1 | FileCheck %s --check-prefix=UNORDSET
// RUN: not emitrust-import-c %t/multimap.cpp 2>&1 | FileCheck %s --check-prefix=MULTIMAP
// RUN: not emitrust-import-c %t/multiset.cpp 2>&1 | FileCheck %s --check-prefix=MULTISET
// RUN: not emitrust-import-c %t/map-comparator.cpp 2>&1 | FileCheck %s --check-prefix=MAPCMP
// RUN: not emitrust-import-c %t/set-comparator.cpp 2>&1 | FileCheck %s --check-prefix=SETCMP
// RUN: not emitrust-import-c %t/float-key.cpp 2>&1 | FileCheck %s --check-prefix=FLOATKEY
// RUN: not emitrust-import-c %t/string-key.cpp 2>&1 | FileCheck %s --check-prefix=STRINGKEY
// RUN: not emitrust-import-c %t/struct-key.cpp 2>&1 | FileCheck %s --check-prefix=STRUCTKEY
// RUN: not emitrust-import-c %t/string-value.cpp 2>&1 | FileCheck %s --check-prefix=STRINGVAL
// RUN: not emitrust-import-c %t/nested-value.cpp 2>&1 | FileCheck %s --check-prefix=NESTEDVAL
// RUN: not emitrust-import-c %t/map-insert.cpp 2>&1 | FileCheck %s --check-prefix=MAPINSERT
// RUN: not emitrust-import-c %t/map-emplace.cpp 2>&1 | FileCheck %s --check-prefix=MAPEMPLACE
// RUN: not emitrust-import-c %t/iterator-decl.cpp 2>&1 | FileCheck %s --check-prefix=ITERDECL
// RUN: not emitrust-import-c %t/iterator-begin.cpp 2>&1 | FileCheck %s --check-prefix=ITERBEGIN
// RUN: not emitrust-import-c %t/iterator-lower-bound.cpp 2>&1 | FileCheck %s --check-prefix=ITERLOWER
// RUN: not emitrust-import-c %t/iterator-stored-find.cpp 2>&1 | FileCheck %s --check-prefix=ITERFIND
// RUN: not emitrust-import-c %t/set-erase-iterator.cpp 2>&1 | FileCheck %s --check-prefix=SETERASEIT
// RUN: not emitrust-import-c %t/at-write.cpp 2>&1 | FileCheck %s --check-prefix=ATWRITE
// RUN: not emitrust-import-c %t/at-compound.cpp 2>&1 | FileCheck %s --check-prefix=ATCOMPOUND
// RUN: not emitrust-import-c %t/at-incr.cpp 2>&1 | FileCheck %s --check-prefix=ATINCR
// RUN: not emitrust-import-c %t/pair-ref-for.cpp 2>&1 | FileCheck %s --check-prefix=PAIRREFFOR
// RUN: not emitrust-import-c %t/binding-by-ref.cpp 2>&1 | FileCheck %s --check-prefix=BINDBYREF
// RUN: not emitrust-import-c %t/map-method.cpp 2>&1 | FileCheck %s --check-prefix=MAPMETHOD
// RUN: not emitrust-import-c %t/set-method.cpp 2>&1 | FileCheck %s --check-prefix=SETMETHOD
// RUN: not emitrust-import-c %t/map-global.cpp 2>&1 | FileCheck %s --check-prefix=MAPGLOBAL
// RUN: not emitrust-import-c %t/map-member.cpp 2>&1 | FileCheck %s --check-prefix=MAPMEMBER
// RUN: not emitrust-import-c %t/map-by-value-param.cpp 2>&1 | FileCheck %s --check-prefix=MAPBYVAL
// RUN: not emitrust-import-c %t/body-uses-range.cpp 2>&1 | FileCheck %s --check-prefix=BODYUSESRANGE

// W2.20 located rejections: every construct explicitly OUT of the
// std::map / std::set surface this wave, plus the two that are out
// PERMANENTLY. Rejection is a feature here — each of these would
// otherwise be either a silent behavior change or a DEFERRED rustc error
// with no source location, and this file is the baseline a later wave
// has to move deliberately.
//
// Why BTreeMap/BTreeSet at all (the framing every rejection below
// inherits): std::map and std::set are ORDERED and their iteration order
// is observable in stdout, so the byte-diff oracle demands the ordered
// Rust container. Measured 2026-08-21: libstdc++ std::map<std::string,int>
// and Rust BTreeMap agree byte-for-byte on an 11-key adversarial set
// including \x7f, \xC8 and \xFF.

//--- unordered-map.cpp
#include <unordered_map>
// PERMANENT, not a backlog item: an unordered container's iteration order
// is UNSPECIFIED, so no Rust container reproduces it byte for byte and
// admitting one would be a silent nondeterminism channel. Its own wording
// (rather than the generic "not a recognized STL type" tail) is what lets
// the rejection ledger tell "deliberately out" from "not done yet".
// UNORDMAP: unordered-map.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unordered_map iteration order is unspecified; no Rust container reproduces it byte for byte
int use(void) {
  std::unordered_map<int, int> m;
  return 0;
}

//--- unordered-set.cpp
#include <unordered_set>
// UNORDSET: unordered-set.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unordered_set iteration order is unspecified; no Rust container reproduces it byte for byte
int use(void) {
  std::unordered_set<int> s;
  return 0;
}

//--- multimap.cpp
#include <map>
// The multi- containers hold DUPLICATE keys; neither BTreeMap nor
// BTreeSet does, and the Rust standard library has no equivalent.
// MULTIMAP: multimap.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::multimap stores duplicate keys; no Rust standard container reproduces it
int use(void) {
  std::multimap<int, int> m;
  return 0;
}

//--- multiset.cpp
#include <set>
// MULTISET: multiset.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::multiset stores duplicate keys; no Rust standard container reproduces it
int use(void) {
  std::multiset<int> s;
  return 0;
}

//--- map-comparator.cpp
#include <map>
#include <functional>
// A custom or reversed comparator keeps the RecordDecl name "map"
// (AST-confirmed 2026-08-21), so the comparator template argument is the
// ONLY screen that catches an ordering BTreeMap does not reproduce.
// MAPCMP: map-comparator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map with a comparator other than std::less
int use(void) {
  std::map<int, int, std::greater<int>> m;
  return 0;
}

//--- set-comparator.cpp
#include <set>
#include <functional>
// SETCMP: set-comparator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::set with a comparator other than std::less
int use(void) {
  std::set<int, std::greater<int>> s;
  return 0;
}

//--- float-key.cpp
#include <map>
// THE ORD SCREEN, which is entirely new code — nothing existing rejects
// this. `rustSpellingForElementType` returns f64 happily, so without the
// screen `std::map<double,int>` would sail through and emit
// `BTreeMap<f64, i32>`, which surfaces as a DEFERRED rustc
// `error[E0277]: the trait bound `f64: Ord` is not satisfied` with no
// source location at all (measured 2026-08-21).
// FLOATKEY: float-key.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map key type 'double' is not in the supported ordered key set
int use(void) {
  std::map<double, int> m;
  return 0;
}

//--- string-key.cpp
#include <map>
#include <string>
// String keys ORDER correctly — measured byte-for-byte against libstdc++
// on an 11-key adversarial set including \x7f, \xC8 and \xFF, because
// char_traits compares unsigned despite plain `char` being signed. The
// blocker is mechanical, not semantic: `BTreeMap::entry(&mut m, k)` MOVES
// the key, so a key read from a local variable needs an inserted clone
// (rustc E0382 otherwise). Out this wave, admissible later.
// STRINGKEY: string-key.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map key type 'class std::basic_string<char>' is not in the supported ordered key set
int use(void) {
  std::map<std::string, int> m;
  return 0;
}

//--- struct-key.cpp
#include <map>
struct K { int a; };
bool operator<(const K &x, const K &y);
// A struct key's Rust image derives Clone/Copy/Default but NOT Ord — the
// same deferred-E0277 channel as a float key.
// STRUCTKEY: struct-key.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map key type 'struct K' is not in the supported ordered key set
int use(void) {
  std::map<K, int> m;
  return 0;
}

//--- string-value.cpp
#include <map>
#include <string>
// THE DEFAULT SCREEN. `m[k]` lowers to `*m.entry(k).or_default()`, which
// needs the value type to implement Rust's `Default` AND to agree with
// C++'s value-initialization. That holds for the integer widths, bool and
// the floats; a String value is admissible in principle but its store and
// load raise a move/clone question this wave does not answer.
// STRINGVAL: string-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map value type 'class std::basic_string<char>' is not in the supported value set
int use(void) {
  std::map<int, std::string> m;
  return 0;
}

//--- nested-value.cpp
#include <map>
#include <vector>
// A nested container value needs a bracket-depth-aware decomposition at
// every element site and raises the same move/clone question at the load.
// NESTEDVAL: nested-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map value type 'class std::vector<int>' is not in the supported value set
int use(void) {
  std::map<int, std::vector<int>> m;
  return 0;
}

//--- map-insert.cpp
#include <map>
// THE MISCOMPILE TRAP. C++'s `map::insert` does NOT overwrite an existing
// key while Rust's `BTreeMap::insert` DOES — measured 2026-08-21: after
// `m[1]=10`, `m.insert({1,99})` leaves m[1]==10 and returns false. A
// naive insert -> insert mapping is silent wrong code, so the site
// rejects; the correct lowering is `entry(k).or_insert(v)`, whose braced
// -pair argument the recognizer cannot spell yet.
// MAPINSERT: map-insert.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::insert does not overwrite an existing key while Rust's BTreeMap::insert does
int use(void) {
  std::map<int, int> m;
  m.insert({1, 2});
  return 0;
}

//--- map-emplace.cpp
#include <map>
// MAPEMPLACE: map-emplace.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::emplace does not overwrite an existing key while Rust's BTreeMap::insert does
int use(void) {
  std::map<int, int> m;
  m.emplace(1, 2);
  return 0;
}

//--- iterator-decl.cpp
#include <map>
// Naming an iterator TYPE. Without a dedicated wording this leaks a
// standard-library-INTERNAL spelling into the diagnostic
// (`std::_Rb_tree_iterator` on libstdc++, `std::__map_iterator` on
// libc++), which no portable test could pin — hence the neutral wording,
// which names the one admitted shape instead.
// ITERDECL: iterator-decl.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map/std::set iterators are only recognized in the find(k) != end() idiom
int use(void) {
  std::map<int, int> m;
  std::map<int, int>::iterator it;
  return 0;
}

//--- iterator-begin.cpp
#include <map>
// ITERBEGIN: iterator-begin.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map/std::set iterators are only recognized in the find(k) != end() idiom
int use(void) {
  std::map<int, int> m;
  m.begin();
  return 0;
}

//--- iterator-lower-bound.cpp
#include <map>
// ITERLOWER: iterator-lower-bound.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map/std::set iterators are only recognized in the find(k) != end() idiom
int use(void) {
  std::map<int, int> m;
  m.lower_bound(1);
  return 0;
}

//--- iterator-stored-find.cpp
#include <map>
// `find(k)` is admitted ONLY as the whole `find(k) != end()` /
// `find(k) == end()` comparison, which materializes no iterator at all.
// Storing the iterator escapes that idiom.
// ITERFIND: iterator-stored-find.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map/std::set iterators are only recognized in the find(k) != end() idiom
int use(void) {
  std::map<int, int> m;
  auto it = m.find(1);
  return it != m.end();
}

//--- set-erase-iterator.cpp
#include <set>
// SETERASEIT: set-erase-iterator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map/std::set iterators are only recognized in the find(k) != end() idiom
int use(void) {
  std::set<int> s;
  s.insert(1);
  s.erase(s.begin());
  return 0;
}

//--- at-write.cpp
#include <map>
// `m.at(k)` lowers to `*std::ops::Index::index(&m, &k)` — a SHARED
// reference place. C++'s at() does return `V&`, so a write through it is
// legal there; rather than let the store reach rustc as a deferred E0594,
// the three write positions reject it here. The entry place is NOT a
// legal substitute: at() must NOT insert.
// ATWRITE: at-write.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::at() is a read-only place; assignment through it is not supported
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  m.at(1) = 3;
  return 0;
}

//--- at-compound.cpp
#include <map>
// ATCOMPOUND: at-compound.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::at() is a read-only place; assignment through it is not supported
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  m.at(1) += 3;
  return 0;
}

//--- at-incr.cpp
#include <map>
// ATINCR: at-incr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::at() is a read-only place; assignment through it is not supported
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  m.at(1)++;
  return 0;
}

//--- pair-ref-for.cpp
#include <map>
// The `pair<const K, V>` REFERENCE form needs a Vec of synthesized Pair
// structs, materially more work than the two scalar places the structured
// -binding form needs. Only `for (auto [k, v] : m)` is admitted.
// PAIRREFFOR: pair-ref-for.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a ranged-for over a std::map requires a structured binding (`for (auto [k, v] : m)`)
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  for (auto &kv : m) {
    (void)kv.first;
  }
  return 0;
}

//--- binding-by-ref.cpp
#include <map>
// A by-REFERENCE structured binding aliases the source object; the
// per-binding copies this lowering builds would miscompile writes. This
// keeps W2.9's existing rejection.
// BINDBYREF: binding-by-ref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: structured binding by reference
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  for (auto &[k, v] : m) {
    (void)k;
    (void)v;
  }
  return 0;
}

//--- map-method.cpp
#include <map>
// Everything outside the pinned table names itself and the receiver
// family — before this wave every non-Vec receiver was reported as a
// std::string or a std::vector, which a map would have made actively
// misleading.
// MAPMETHOD: map-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map::swap is not a recognized STL method
int use(void) {
  std::map<int, int> m;
  m.swap(m);
  return 0;
}

//--- set-method.cpp
#include <set>
// SETMETHOD: set-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::set::max_size is not a recognized STL method
int use(void) {
  std::set<int> s;
  s.max_size();
  return 0;
}

//--- map-global.cpp
#include <map>
// A map/set may only be a LOCAL or a REFERENCE. W2.17's destructor guards
// already fence the three dangerous positions for free; pinned here so a
// later wave cannot widen the type mapping without noticing that the
// Copy-derive predicate would then derive Copy on a BTreeMap-carrying
// struct (rustc E0204).
// MAPGLOBAL: map-global.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
std::map<int, int> g;
int use(void) { return 0; }

//--- map-member.cpp
#include <map>
// MAPMEMBER: map-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
struct S {
  std::map<int, int> m;
};
int use(void) {
  S s;
  return 0;
}

//--- map-by-value-param.cpp
#include <map>
// MAPBYVAL: map-by-value-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
int take(std::map<int, int> m) { return (int)m.size(); }

//--- body-uses-range.cpp
#include <map>
// W2.10's R3 restriction stays verbatim: the desugar re-reads len() every
// iteration, and the ordered key snapshot is a SEPARATE Vec, so a body
// that mutated the map would borrow-check but diverge from C++'s live
// -iteration semantics.
// BODYUSESRANGE: body-uses-range.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ranged-for body may not use the range variable
int use(void) {
  std::map<int, int> m;
  m[1] = 2;
  for (auto [k, v] : m) {
    (void)k;
    (void)v;
    (void)m.size();
  }
  return 0;
}
