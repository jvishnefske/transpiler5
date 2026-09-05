// REQUIRES: cargo
// FR-196: A FIELD WRITE THROUGH A std::vector ELEMENT PLACE MUST LAND, AND
// A BYTE-DIFF IS THE ONLY ORACLE THAT CAN SAY SO.
//
// Before FR-196 the whole assignment statement `v[0].x = 99;` DISAPPEARED
// from the emitted crate. `v[i]` is a `CXXOperatorCallExpr` -- and
// `v.at(i)` / `v.front()` / `v.back()` are `CXXMemberCallExpr`s -- so all
// of them are `clang::CallExpr`s, and `emitMemberBasePlace`'s `f().m`
// branch claimed the base before anything else: it LOADED the element out
// of the vector into a fresh temporary and projected the member out of the
// COPY. The copy was never read back, so dead-store elimination then
// deleted the staging load AND the store, and the statement emitted as a
// bare `let _v6: P;`. Measured on the minimal shape: clang++ printed `99`,
// the emitted crate printed `1`, and `cargo build` was CLEAN.
//
// That is strictly worse than the FR-189 defect it rhymes with. There
// (`(*p).field = v` on a std::unique_ptr payload) the store at least
// landed on the copy, so the program carried a wrong VALUE. Here the store
// is gone entirely -- and right-hand-side effects are still emitted, so a
// `printf` in the RHS still fires and the program looks alive while the
// write is simply absent. This repo's rule is that a construct it cannot
// handle gets a LOCATED rejection; silently dropping a store is the exact
// failure mode that rule exists to forbid.
//
// A FileCheck cannot see a dropped store -- it can only see the ops that
// ARE emitted -- which is how this survived a green gate. Hence the pin is
// here, as a diff against the clang++ native.
//
// WHAT EACH LEG PINS (every spelling of the element place is affected, so
// every spelling is here):
//   * `v[i].f` -- the headline, plain store and compound store.
//   * `v.at(i).f`, `v.front().f`, `v.back().f` -- the member-call
//     spellings of the same place. `at` is bounds-checked in C++ and
//     `front`/`back` are not, but all three are `T&` and must resolve to
//     the identical `emitrust.subscript` place.
//   * NESTED projections off the element: `v[i].in.a` (member of member),
//     `v[i].arr[k]` (array member subscript) and `v[i].z++` / `--`, whose
//     read-modify-write goes through the same place twice.
//   * DOUBLY INDEXED `g[i][j].f` on a `vector<vector<P>>` -- the outer
//     element place is itself resolved recursively through the new path.
//   * The REALISTIC shape: `for (i..) v[i].count = v[i].id * argc + 1;`
//     followed by a sum. This printed `total=10` natively and `total=0`
//     from the emitted crate; it is the form real code takes.
//   * A CROSS-READ `v[0].x = v[1].y + argc`, which is the shape most
//     likely to trip Rust's borrow checker on the way to a fix (an
//     `IndexMut` place held across an `Index` read).
//
// AND THE FOUR ADJACENT SHAPES THAT ALREADY WORKED ARE HERE AS CONTROLS,
// deliberately in this same file: a scalar element write `si[i] = x`, a
// WHOLE-element write `v[i] = q`, a mutating range-for `for (P &p : v)
// p.x *= 2;`, and a mutating method call `cs[0].bump(5)`. They are what
// localised the defect to member projection rather than to vector
// mutation in general, so a "fix" that repairs the field write by
// disturbing them fails right here rather than somewhere downstream.
//
// Every printed value derives from argc, so constant folding cannot
// pre-compute the answers and hide a miscompile behind a compile-clean
// crate. argc only -- no argv.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/stl_vector_element_member_write > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

#include <array>
#include <vector>

struct Inner {
  int a;
};

struct P {
  int x;
  int y;
  Inner in;
  int arr[3];
  int z;
};

struct Q {
  int a;
  int b;
};

struct Item {
  int id;
  int count;
};

struct Counter {
  int n;
  void bump(int d) { n += d; }
};

static P makeP(int seed) {
  P p;
  p.x = seed;
  p.y = seed + 1;
  p.in.a = seed + 2;
  p.arr[0] = seed + 3;
  p.arr[1] = seed + 4;
  p.arr[2] = seed + 5;
  p.z = seed + 6;
  return p;
}

static void dump(const char *tag, const std::vector<P> &v) {
  for (unsigned i = 0; i < v.size(); i++)
    printf("%s[%u] %d %d %d %d %d %d %d\n", tag, i, v[i].x, v[i].y, v[i].in.a,
           v[i].arr[0], v[i].arr[1], v[i].arr[2], v[i].z);
}

// The headline: a plain and a compound field store through `v[i]`.
static void subscript_writes(int argc) {
  std::vector<P> v;
  v.push_back(makeP(argc));
  v.push_back(makeP(argc + 10));
  v[0].x = argc + 99;
  v[1].y += argc;
  dump("sub", v);
}

// The member-call spellings of the same place.
static void member_call_writes(int argc) {
  std::vector<P> v;
  v.push_back(makeP(argc));
  v.push_back(makeP(argc + 10));
  v.push_back(makeP(argc + 20));
  v.at(1).x = argc + 40;
  v.at(1).y += argc + 1;
  v.front().x = argc + 50;
  v.back().y = argc + 60;
  v.back().z += argc;
  dump("mc", v);
}

// Nested member, array-member subscript, and the read-modify-write forms.
static void nested_writes(int argc) {
  std::vector<P> v;
  v.push_back(makeP(argc));
  v[0].in.a = argc + 70;
  v[0].arr[1] = argc + 71;
  v[0].arr[argc - argc + 2] = argc + 72;
  v[0].z++;
  v[0].z++;
  v[0].z--;
  ++v[0].x;
  dump("nest", v);
}

// `g[i][j].f`: the outer element place is itself an element place.
static void doubly_indexed(int argc) {
  std::vector<std::vector<P>> g;
  for (int i = 0; i < 2; i++) {
    std::vector<P> row;
    for (int j = 0; j < 2; j++)
      row.push_back(makeP(argc + i * 4 + j));
    g.push_back(row);
  }
  g[0][1].x = argc + 88;
  g[1][0].y += argc + 9;
  g[1][1].in.a = argc + 7;
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++)
      printf("g%d%d %d %d %d\n", i, j, g[i][j].x, g[i][j].y, g[i][j].in.a);
}

// The shape real code takes, and the shape that printed `total=0`.
static void loop_write(int argc) {
  std::vector<Item> v;
  for (int i = 0; i < 4; i++) {
    Item it;
    it.id = i;
    it.count = 0;
    v.push_back(it);
  }
  for (unsigned i = 0; i < v.size(); i++)
    v[i].count = v[i].id * argc + 1;
  int total = 0;
  for (unsigned i = 0; i < v.size(); i++) {
    printf("item %u %d %d\n", i, v[i].id, v[i].count);
    total += v[i].count;
  }
  printf("total=%d\n", total);
}

// An `IndexMut` destination fed from an `Index` read of the same vector.
static void cross_read(int argc) {
  std::vector<P> v;
  v.push_back(makeP(argc));
  v.push_back(makeP(argc + 10));
  v[0].x = v[1].y + argc;
  v[1].z += v[0].x;
  dump("cross", v);
}

// CONTROL 1: a scalar element write. Worked before FR-196.
static void control_scalar(int argc) {
  std::vector<int> si;
  si.push_back(argc);
  si.push_back(argc + 1);
  si[0] = argc + 30;
  si[1] += argc;
  printf("scal %d %d\n", si[0], si[1]);
}

// CONTROL 2: a WHOLE-element write. Worked before FR-196. Uses the flat
// `Q` rather than `P` because implicit copy assignment of a class with
// non-scalar members is a separate, pre-existing located rejection --
// unrelated to this defect, and not something to pin here.
static void control_whole_element(int argc) {
  std::vector<Q> v;
  Q first;
  first.a = argc;
  first.b = argc + 1;
  v.push_back(first);
  Q q;
  q.a = argc + 100;
  q.b = argc + 101;
  v[0] = q;
  printf("whole %d %d\n", v[0].a, v[0].b);
}

// CONTROL 3: a mutating range-for over references. Worked before FR-196.
static void control_range_for(int argc) {
  std::vector<P> v;
  v.push_back(makeP(argc));
  v.push_back(makeP(argc + 10));
  for (P &p : v)
    p.x *= 2;
  dump("rfor", v);
}

// A std::array<P, N> element place is an `!emitrust.array<NxP>`
// subscript, and `a[i].f = x` reached the SAME `f().m` branch through the
// same `CXXOperatorCallExpr` -- but there it did not silently drop,
// because `emitCall` had no lowering for the array operator and rejected
// with a located diagnostic ("operator call receiver is not a recognized
// STL type"). Routing the base through `emitLValue` makes it WORK, so
// this leg is a capability gain riding on the same fix and is pinned
// here so it cannot regress back to that rejection unnoticed.
// (`a.at(i).f` / `a.front().f` / `a.back().f` stay rejected; see
// test/Import/Cpp/stl-vector-element-member-invalid.cpp.)
static void array_element_member(int argc) {
  std::array<P, 2> a{};
  a[0].x = argc + 20;
  a[1].y = argc + 21;
  a[0].in.a = argc + 22;
  a[1].arr[2] = argc + 23;
  a[argc - argc + 1].z++;
  printf("arr %d %d %d %d %d %d\n", a[0].x, a[0].in.a, a[1].y, a[1].arr[2],
         a[1].z, a[0].z);
}

// CONTROL 4: a mutating METHOD call on an element. Worked before FR-196.
static void control_method_call(int argc) {
  std::vector<Counter> cs;
  Counter c;
  c.n = argc;
  cs.push_back(c);
  cs[0].bump(argc + 5);
  printf("meth %d\n", cs[0].n);
}

int main(int argc, char **) {
  subscript_writes(argc);
  member_call_writes(argc);
  nested_writes(argc);
  doubly_indexed(argc);
  loop_write(argc);
  cross_read(argc);
  array_element_member(argc);
  control_scalar(argc);
  control_whole_element(argc);
  control_range_for(argc);
  control_method_call(argc);
  return 0;
}
