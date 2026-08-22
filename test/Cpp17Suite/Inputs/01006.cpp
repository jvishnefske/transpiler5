#include <map>
#include <set>
#include <iostream>

// W2.20: the first corpus entry over ORDERED associative containers.
//
// BTreeMap/BTreeSet, not HashMap/HashSet: std::map and std::set are
// ORDERED and their iteration order is observable in stdout, so the
// byte-diff oracle demands the ordered Rust container. Measured
// 2026-08-21: libstdc++ std::map<std::string,int> and Rust BTreeMap agree
// byte-for-byte on an 11-key adversarial set including \x7f, \xC8 and
// \xFF.
//
// The hazards this entry exists to catch, none of which a compile-clean
// `cargo build` can see:
//  - operator[] DEFAULT-INSERTS on a missing key and returns a reference
//    to the new element, so `m[k]` is a MUTATION even in a read position:
//    `int miss = m[99];` inserts 99 and m.size() observes it. Rust's
//    `Index` PANICS there, so the read, the write, the compound and the
//    missing-key read must ALL lower to `*m.entry(k).or_default()`.
//  - C++17 (P0145R3) sequences the RHS of `E1 = E2` BEFORE E1, so the
//    entry place must be created AFTER the right-hand side is a value.
//    Measured: `a[1] = (int)a.size()` prints 0 on clang++ and g++.
//  - erase(k) returns the number of ELEMENTS removed (0 or 1), not a
//    bool-of-success, and erasing a key that operator[] just default
//    -inserted must report 1.
//  - `for (auto [k, v] : m)` must traverse in KEY ORDER; the keys were
//    inserted 1, 3, 2, then 1 (compound), then 4 (default-insert) on
//    purpose, so an insertion-ordered container would print a different
//    permutation.
//  - std::set::insert ignores a duplicate (s.insert(9) twice leaves
//    size 3), matching BTreeSet::insert exactly.
//
// Every value is derived from argc so no constant folding can pre-compute
// the answers; argv itself is never touched.

int main(int argc, char **) {
  std::map<int, int> m;
  m[1] = 10 * argc;
  m[3] = 30;
  m[2] = 20;
  m[1] += 5;
  m[4]++;
  int r = m[3];
  int miss = m[99];
  std::cout << "r=" << r << " miss=" << miss << " size=" << (int)m.size() << "\n";
  std::cout << "count2=" << (int)m.count(2) << " count77=" << (int)m.count(77) << "\n";
  std::cout << "at2=" << m.at(2) << "\n";
  std::cout << "erase99=" << (int)m.erase(99) << " erase77=" << (int)m.erase(77) << "\n";
  std::cout << "find4=" << (int)(m.find(4) != m.end()) << " find77=" << (int)(m.find(77) != m.end()) << "\n";
  for (auto [k, v] : m) std::cout << "kv " << k << "=" << v << "\n";
  std::cout << "empty=" << (int)m.empty() << "\n";
  m.clear();
  std::cout << "cleared=" << (int)m.size() << " empty=" << (int)m.empty() << "\n";

  std::set<int> s;
  s.insert(9);
  s.insert(2);
  s.insert(9);
  s.insert(5 * argc);
  std::cout << "ssize=" << (int)s.size() << " has2=" << (int)s.count(2) << " has7=" << (int)s.count(7) << "\n";
  for (int x : s) std::cout << "s " << x << "\n";
  s.erase(2);
  std::cout << "after=" << (int)s.size() << "\n";
  return 0;
}
