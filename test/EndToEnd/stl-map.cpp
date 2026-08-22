// REQUIRES: cargo
// W2.20: std::map -> BTreeMap and std::set -> BTreeSet, end to end. THE
// oracle for the wave: the emitted crate's stdout is diffed byte for byte
// against a `clang++ -std=c++17` build of the identical source.
//
// BTreeMap/BTreeSet, not HashMap/HashSet: std::map and std::set are
// ORDERED and their iteration order is observable in stdout, so the
// byte-diff oracle demands the ordered Rust container. Measured
// 2026-08-21: libstdc++ std::map<std::string,int> and Rust BTreeMap agree
// byte-for-byte on an 11-key adversarial set including \x7f, \xC8 and
// \xFF. A HashMap here would print a permutation and this diff would be
// the only thing in the project able to see it.
//
// What each line is here to catch — every one of them is invisible to a
// compile-clean `cargo build`:
//  - THE operator[] TRAP. C++ `m[k]` on a MISSING key DEFAULT-INSERTS and
//    hands back a reference to the new element, so `m[k]` mutates even in
//    a READ position (`int miss = m[99];` inserts 99, and `m.size()`
//    observes it). Rust's `Index` PANICS on a missing key, so all four
//    spellings — read, write, compound, missing-key read — must lower to
//    `*m.entry(k).or_default()`. `miss`/`size` below pin exactly that.
//  - C++17 EVALUATION ORDER (P0145R3): in `E1 = E2` and `E1 op= E2`, E2 is
//    sequenced BEFORE E1, so the entry place must be built AFTER the
//    right-hand side is already a value. `seq[1] = (int)seq.size()` prints
//    0 on clang++ AND g++; a naive LHS-place-first lowering prints 1 (and
//    also trips rustc E0499 whenever two map places are live at once).
//  - ORDERED ITERATION. The keys are inserted out of order on purpose
//    (1, 3, 2, then a compound on 1, then a default-insert of 4), so an
//    insertion-ordered or hash-ordered container prints a different
//    permutation of the same lines.
//  - erase(k) returns the COUNT removed (0 or 1), not a success bool, and
//    at() panics/throws only on a key that is absent — so the admitted
//    at() call below is on a key that is provably present.
//  - std::set::insert IGNORES a duplicate and returns false; Rust's
//    BTreeSet::insert has the identical contract, and the duplicate
//    insert below pins it.
//
// Every value derives from argc, so no constant folding can pre-compute
// the answers and hide a miscompile behind a compile-clean crate. argv is
// a hard rejection here — argc only.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_map > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <map>
#include <set>

int map_surface(int seed) {
  std::map<int, int> m;
  m[1] = 10 * seed;      // write to a missing key
  m[3] = 30 * seed;      // ditto, out of key order
  m[2] = 20 * seed;      // ditto
  m[1] += 5 * seed;      // compound on a PRESENT key
  m[4] += seed;          // compound on a MISSING key: default-insert then +=
  int r = m[3];          // plain read of a present key
  int miss = m[99];      // READ of a MISSING key: inserts 99 -> 0
  int size = (int)m.size();
  int c2 = (int)m.count(2);
  int c77 = (int)m.count(77);
  int at2 = m.at(2);
  int e99 = (int)m.erase(99);  // 99 exists only because m[99] inserted it
  int e77 = (int)m.erase(77);
  int f4 = (int)(m.find(4) != m.end());
  int f77 = (int)(m.find(77) == m.end());
  int keysum = 0;
  int valsum = 0;
  for (auto [k, v] : m) {
    printf("kv %d=%d\n", k, v);
    keysum = keysum * 10 + k;
    valsum += v;
  }
  int was_empty = (int)m.empty();
  m.clear();
  int now_empty = (int)m.empty();
  printf("r=%d miss=%d size=%d c2=%d c77=%d at2=%d\n", r, miss, size, c2, c77,
         at2);
  printf("e99=%d e77=%d f4=%d f77=%d keysum=%d valsum=%d we=%d ne=%d n=%d\n",
         e99, e77, f4, f77, keysum, valsum, was_empty, now_empty,
         (int)m.size());
  return r + miss + size + at2 + keysum + valsum;
}

int set_surface(int seed) {
  std::set<int> s;
  s.insert(9 * seed);
  s.insert(2 * seed);
  s.insert(9 * seed); // duplicate: ignored on both sides
  s.insert(5 * seed);
  s.insert(seed);
  int n = (int)s.size();
  int has2 = (int)s.count(2 * seed);
  int has7 = (int)s.count(7 * seed);
  int order = 0;
  for (int x : s) {
    printf("s %d\n", x);
    order = order * 100 + x;
  }
  int erased = (int)s.erase(2 * seed);
  int again = (int)s.erase(2 * seed);
  int empty = (int)s.empty();
  printf("n=%d has2=%d has7=%d order=%d erased=%d again=%d empty=%d left=%d\n",
         n, has2, has7, order, erased, again, empty, (int)s.size());
  s.clear();
  printf("cleared=%d\n", (int)s.size());
  return n + order + erased;
}

// C++17 P0145R3: E2 is sequenced BEFORE E1 in `E1 = E2`, so the size()
// read below happens BEFORE the default-insert of key 1. clang++ and g++
// both print 0 here; an LHS-place-first lowering prints 1.
int sequencing(int seed) {
  std::map<int, int> seq;
  seq[1] = (int)seq.size();
  int first = seq[1];
  seq[2] = seq[1] + seed;
  seq[2] += (int)seq.size();
  int second = seq[2];
  int total = (int)seq.size();
  printf("first=%d second=%d total=%d\n", first, second, total);
  return first + second + total;
}

int main(int argc, char **) {
  int a = map_surface(argc);
  int b = map_surface(argc + 1);
  int c = set_surface(argc);
  int d = set_surface(argc + 2);
  int e = sequencing(argc);
  printf("%d %d %d %d %d\n", a, b, c, d, e);
  return 0;
}
