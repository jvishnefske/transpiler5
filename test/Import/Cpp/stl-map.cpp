// RUN: emitrust-import-c %s | FileCheck %s

// W2.20: STL recognition for `std::map<K, V>` and `std::set<K>` USAGE.
//
// BTreeMap/BTreeSet, not HashMap/HashSet: std::map and std::set are
// ORDERED and their iteration order is observable in stdout, so the
// byte-diff oracle demands the ordered Rust container. Measured
// 2026-08-21: libstdc++ `std::map<std::string,int>` and Rust
// `BTreeMap<String,i32>` agree byte-for-byte on an 11-key adversarial set
// including \x7f, \xC8 and \xFF (libstdc++'s char_traits compares
// unsigned, so the signed-char trap does not exist). A HashMap image
// would be a silent nondeterminism channel; see stl-map-invalid.cpp for
// the permanent std::unordered_map rejection that follows from the same
// argument.
//
// What this file pins, and why each shape is what it is:
//
//  * THE operator[] PLACE. C++'s `m[k]` on a MISSING key DEFAULT-INSERTS
//    and returns a reference to the new element, so `m[k]` is a MUTATION
//    even in a READ position (`int r = m[1];` on an absent key inserts it
//    and `m.size()` observes the insert). Rust's `Index` PANICS there, so
//    a plain `emitrust.subscript` place would be a miscompile. All four
//    C++ spellings — write, compound, read, and missing-key read — share
//    ONE place: `addr_of mut` + `BTreeMap::entry` + `Entry::or_default` +
//    `deref`. The UFCS free-call spelling is forced: `emitrust.method_call`
//    renders `place.method(args)` and needs an LVALUE receiver, so
//    `m.entry(k).or_default()` is not expressible as method calls at all.
//
//  * count()/erase() take the key BY REFERENCE, and `&<temporary>` has no
//    place to borrow from, so each materializes an unnamed
//    `emitrust.variable` key cell first. erase() returns the COUNT removed
//    (0 or 1), not a success flag, so a `contains_key` probe runs FIRST
//    and the `remove` itself is discarded — typed `Option<V>` rather than
//    result-less so the emitter's `_vN` naming absorbs Option's
//    `#[must_use]`.
//
//  * at() is the READ-ONLY `std::ops::Index::index(&m, &k)` place; Rust's
//    panic on a missing key refines C++'s std::out_of_range throw exactly
//    the way W2.6 argued for vector front()/back() on an empty vector.
//
//  * `m.find(k) != m.end()` is the ONE iterator shape admitted, and it
//    lowers to `contains_key(&k)` with NO iterator materialized; the
//    `== m.end()` spelling is the same probe xor'd with true.
//
//  * ORDERED ITERATION. BTreeMap/BTreeSet have no positional index, so
//    W2.10's index desugar cannot walk them. The lowering snapshots the
//    keys in order — `Vec::from_iter(BTreeMap::keys(&m).cloned())`, every
//    intermediate iterator type NAMEABLE because the emitter's `let`
//    prologue demands an annotation — and then runs the EXISTING index
//    loop over that Vec. `for (auto [k, v] : m)` registers the two
//    BindingDecls as separate scalar places, the value looked up by key.
//
//  * The two `emitrust.use` lines are emitted once per module on first
//    mention, in first-mention order.

// CHECK: emitrust.use "std::collections::BTreeMap"
// CHECK: emitrust.use "std::collections::BTreeSet"

#include <map>
#include <set>

// CHECK-LABEL: func.func @use_map
int use_map(void) {
  // CHECK: %[[M:.*]] = emitrust.variable named "m" : !emitrust.lvalue<!emitrust.opaque<"BTreeMap<i32, i32>">>
  // CHECK: emitrust.call_opaque "BTreeMap::new"() : () -> !emitrust.opaque<"BTreeMap<i32, i32>">
  std::map<int, int> m;
  // The WRITE spelling: entry place, then the store into it.
  // CHECK: %[[R1:.*]] = emitrust.addr_of mut %[[M]] : {{.*}} -> !emitrust.mut_ref<!emitrust.opaque<"BTreeMap<i32, i32>">>
  // CHECK: %[[E1:.*]] = emitrust.call_opaque "std::collections::BTreeMap::entry"(%[[R1]], %{{.*}}) : {{.*}} -> !emitrust.opaque<"std::collections::btree_map::Entry<'_, i32, i32>">
  // CHECK: %[[S1:.*]] = emitrust.call_opaque "std::collections::btree_map::Entry::or_default"(%[[E1]]) : {{.*}} -> !emitrust.mut_ref<i32>
  // CHECK: %[[P1:.*]] = emitrust.deref %[[S1]] : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[P1]] = %{{.*}} : !emitrust.lvalue<i32>
  m[1] = 10;
  // The COMPOUND spelling: the SAME entry place, loaded and stored back.
  // CHECK: %[[E2:.*]] = emitrust.call_opaque "std::collections::BTreeMap::entry"
  // CHECK: %[[S2:.*]] = emitrust.call_opaque "std::collections::btree_map::Entry::or_default"(%[[E2]])
  // CHECK: %[[P2:.*]] = emitrust.deref %[[S2]]
  // CHECK: %[[L2:.*]] = emitrust.load %[[P2]]
  // CHECK: %[[A2:.*]] = arith.addi %[[L2]], %{{.*}}
  // CHECK: emitrust.assign %[[P2]] = %[[A2]]
  m[2] += 3;
  // The READ spelling: the SAME mutating entry place, then a load. This
  // is the trap — a read of a missing key inserts it in C++ too.
  // CHECK: %[[E3:.*]] = emitrust.call_opaque "std::collections::BTreeMap::entry"
  // CHECK: %[[S3:.*]] = emitrust.call_opaque "std::collections::btree_map::Entry::or_default"(%[[E3]])
  // CHECK: %[[P3:.*]] = emitrust.deref %[[S3]]
  // CHECK: emitrust.load %[[P3]]
  int r = m[1];
  // size() reuses the family-agnostic len() lambda: index-typed, then
  // cast to the call expression's OWN declared C type (size_t -> ui64).
  // CHECK: %[[N:.*]] = emitrust.method_call %[[M]]["len"] () : {{.*}} -> index
  // CHECK: emitrust.cast %[[N]] : index to ui64
  int n = (int)m.size();
  // CHECK: emitrust.method_call %[[M]]["is_empty"] () : {{.*}} -> i1
  int e = (int)m.empty();
  // count(k): the materialized key cell, its shared reference, and a
  // contains_key probe cast to the declared size_t.
  // CHECK: %[[KC:.*]] = emitrust.variable : !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[KC]] = %{{.*}}
  // CHECK: %[[KR:.*]] = emitrust.addr_of %[[KC]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: %[[HIT:.*]] = emitrust.method_call %[[M]]["contains_key"] (%[[KR]]) : {{.*}} -> i1
  // CHECK: emitrust.cast %[[HIT]] : i1 to ui64
  int c = (int)m.count(2);
  // at(k): the READ-ONLY shared-reference place, never the entry place.
  // CHECK: %[[MR:.*]] = emitrust.addr_of %[[M]] : {{.*}} -> !emitrust.ref<!emitrust.opaque<"BTreeMap<i32, i32>">>
  // CHECK: %[[AK:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: %[[IX:.*]] = emitrust.call_opaque "std::ops::Index::index"(%[[MR]], %[[AK]]) : {{.*}} -> !emitrust.ref<i32>
  // CHECK: %[[AP:.*]] = emitrust.deref %[[IX]] : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  // CHECK: emitrust.load %[[AP]]
  int a = m.at(1);
  // erase(k): the count comes from the probe; the removal is discarded
  // through an Option<V>-typed result, and BOTH calls share one key ref.
  // CHECK: %[[EK:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: %[[HAD:.*]] = emitrust.method_call %[[M]]["contains_key"] (%[[EK]]) : {{.*}} -> i1
  // CHECK: emitrust.method_call %[[M]]["remove"] (%[[EK]]) : {{.*}} -> !emitrust.opaque<"Option<i32>">
  // CHECK: emitrust.cast %[[HAD]] : i1 to ui64
  int d = (int)m.erase(2);
  // `find(k) != end()`: contains_key, no iterator anywhere.
  // CHECK: %[[F:.*]] = emitrust.method_call %[[M]]["contains_key"] (%{{.*}}) : {{.*}} -> i1
  // CHECK: arith.extui %[[F]] : i1 to i32
  int f = (int)(m.find(1) != m.end());
  // `find(k) == end()`: the same probe, negated.
  // CHECK: %[[G:.*]] = emitrust.method_call %[[M]]["contains_key"] (%{{.*}}) : {{.*}} -> i1
  // CHECK: %[[TRUE:.*]] = arith.constant true
  // CHECK: %[[NG:.*]] = arith.xori %[[G]], %[[TRUE]] : i1
  // CHECK: arith.extui %[[NG]] : i1 to i32
  int g = (int)(m.find(9) == m.end());
  // CHECK: emitrust.method_call %[[M]]["clear"] () : {{.*}} -> ()
  m.clear();
  return r + n + e + c + a + d + f + g;
}

// CHECK-LABEL: func.func @use_set
int use_set(void) {
  // CHECK: %[[S:.*]] = emitrust.variable named "s" : !emitrust.lvalue<!emitrust.opaque<"BTreeSet<i32>">>
  // CHECK: emitrust.call_opaque "BTreeSet::new"() : () -> !emitrust.opaque<"BTreeSet<i32>">
  std::set<int> s;
  // std::set::insert and BTreeSet::insert agree EXACTLY: neither replaces
  // an existing element, and both report whether the set changed
  // (measured 2026-08-21). The report is unused in statement position.
  // CHECK: emitrust.method_call %[[S]]["insert"] (%{{.*}}) : {{.*}} -> ()
  s.insert(4);
  // CHECK: emitrust.method_call %[[S]]["len"] () : {{.*}} -> index
  int n = (int)s.size();
  // A set probes with `contains`, not `contains_key`.
  // CHECK: emitrust.method_call %[[S]]["contains"] (%{{.*}}) : {{.*}} -> i1
  int h = (int)s.count(4);
  // erase(k) on a set discards a plain bool rather than an Option.
  // CHECK: %[[SK:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: %[[SHAD:.*]] = emitrust.method_call %[[S]]["contains"] (%[[SK]]) : {{.*}} -> i1
  // CHECK: emitrust.method_call %[[S]]["remove"] (%[[SK]]) : {{.*}} -> i1
  int d = (int)s.erase(4);
  // CHECK: emitrust.method_call %[[S]]["clear"] () : {{.*}} -> ()
  s.clear();
  return n + h + d;
}

// CHECK-LABEL: func.func @iterate
int iterate(void) {
  std::map<int, int> m;
  m[1] = 5;
  int total = 0;
  // The ORDERED KEY SNAPSHOT, then the existing index loop over it.
  // CHECK: %[[MP:.*]] = emitrust.addr_of %{{.*}} : {{.*}} -> !emitrust.ref<!emitrust.opaque<"BTreeMap<i32, i32>">>
  // CHECK: %[[KS:.*]] = emitrust.call_opaque "std::collections::BTreeMap::keys"(%[[MP]]) : {{.*}} -> !emitrust.opaque<"std::collections::btree_map::Keys<'_, i32, i32>">
  // CHECK: %[[CL:.*]] = emitrust.call_opaque "std::iter::Iterator::cloned"(%[[KS]]) : {{.*}} -> !emitrust.opaque<"std::iter::Cloned<std::collections::btree_map::Keys<'_, i32, i32>>">
  // CHECK: %[[VEC:.*]] = emitrust.call_opaque "Vec::from_iter"(%[[CL]]) : {{.*}} -> !emitrust.opaque<"Vec<i32>">
  // CHECK: %[[SNAP:.*]] = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Vec<i32>">>
  // CHECK: emitrust.assign %[[SNAP]] = %[[VEC]]
  // CHECK: emitrust.method_call %[[SNAP]]["len"] () : {{.*}} -> index
  // The key binding is a per-iteration COPY of the snapshot element; the
  // value binding is looked up in the map BY that key.
  // CHECK: %[[EL:.*]] = emitrust.subscript %[[SNAP]][%{{.*}}] : {{.*}} -> !emitrust.lvalue<i32>
  // CHECK: %[[KV:.*]] = emitrust.load %[[EL]]
  // CHECK: %[[KB:.*]] = emitrust.variable named "k" : !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[KB]] = %[[KV]]
  // CHECK: %[[MR2:.*]] = emitrust.addr_of %{{.*}} : {{.*}} -> !emitrust.ref<!emitrust.opaque<"BTreeMap<i32, i32>">>
  // CHECK: %[[ER:.*]] = emitrust.addr_of %[[EL]] : (!emitrust.lvalue<i32>) -> !emitrust.ref<i32>
  // CHECK: %[[IV:.*]] = emitrust.call_opaque "std::ops::Index::index"(%[[MR2]], %[[ER]]) : {{.*}} -> !emitrust.ref<i32>
  // CHECK: %[[VP:.*]] = emitrust.deref %[[IV]] : (!emitrust.ref<i32>) -> !emitrust.lvalue<i32>
  // CHECK: %[[VV:.*]] = emitrust.load %[[VP]]
  // CHECK: %[[VB:.*]] = emitrust.variable named "v" : !emitrust.lvalue<i32>
  // CHECK: emitrust.assign %[[VB]] = %[[VV]]
  for (auto [k, v] : m) total += k * v;
  std::set<int> s;
  s.insert(2);
  // The set form snapshots through BTreeSet::iter and binds the single
  // loop variable to a copy of the snapshot element.
  // CHECK: %[[SP:.*]] = emitrust.addr_of %{{.*}} : {{.*}} -> !emitrust.ref<!emitrust.opaque<"BTreeSet<i32>">>
  // CHECK: %[[SI:.*]] = emitrust.call_opaque "std::collections::BTreeSet::iter"(%[[SP]]) : {{.*}} -> !emitrust.opaque<"std::collections::btree_set::Iter<'_, i32>">
  // CHECK: %[[SC:.*]] = emitrust.call_opaque "std::iter::Iterator::cloned"(%[[SI]]) : {{.*}} -> !emitrust.opaque<"std::iter::Cloned<std::collections::btree_set::Iter<'_, i32>>">
  // CHECK: emitrust.call_opaque "Vec::from_iter"(%[[SC]]) : {{.*}} -> !emitrust.opaque<"Vec<i32>">
  // CHECK: emitrust.variable named "x" : !emitrust.lvalue<i32>
  for (int x : s) total += x;
  return total;
}

// W2.20: a map/set may only be a LOCAL or a REFERENCE — W2.17's
// destructor guards already reject the global, struct-member and
// by-value parameter/return positions (see stl-map-invalid.cpp). The
// by-REFERENCE parameter composes with FR-48 for free and renders a
// correct `&mut BTreeMap<i32, i32>` reborrow.
// CHECK-LABEL: func.func @bump
void bump(std::map<int, int> &m, int k) {
  // CHECK: emitrust.call_opaque "std::collections::BTreeMap::entry"
  // CHECK: emitrust.call_opaque "std::collections::btree_map::Entry::or_default"
  m[k] += 1;
}
