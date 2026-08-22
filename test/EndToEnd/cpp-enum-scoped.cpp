// REQUIRES: cargo
// FR-113: scoped enumerations (enum class/struct) import as their
// underlying-typed C-like image -- the same open-enum emitrust.enum_def an
// unscoped enum gets; scoping is compile-time namespacing the resolved call
// sites already discharged. This differential leg pins every admitted shape
// end to end: enumerator access (`Color c = Color::Green;`, the C1 idiom
// that used to reject with a misleading type mismatch), ==/!= comparisons,
// a switch with scoped case labels, static_cast in BOTH directions
// including an OUT-OF-RANGE value through a narrow `unsigned char`
// underlying type (C++ truncates 301 to 45; enum_def storage is fixed
// u32/i32, so the importer must insert the intermediate `as u8` cast),
// enum-typed params/returns/fields, an enumerator as a call argument, and a
// cast-to-enum inside a class METHOD (emitrust.impl is a SymbolTable, so
// the emitter's enum lookup must resolve module-level enum_defs from inside
// it -- EnumDefOp::lookupFrom, the FR-84 pattern). `probe` is the 4-line
// spdlog Spec/PT repro that motivated the FR. All values derive from argc
// so constant folding cannot hide a miscompile; branchy enum-typed returns
// (the scf.if/scf.index_switch merge shapes) get their own leg in
// test/EndToEnd/cpp-enum-branchy.cpp.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_enum_scoped > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

enum class PT : unsigned char { A = 10, B = 45 };
enum class Color { Red, Green, Blue = 7 };

struct Spec {
  PT pt;
  int width;
};

// Enum-typed parameter and return (assign-then-single-return shape).
static Color pick(Color c, int n) {
  Color r = Color::Blue;
  if (n > 1) {
    r = c;
  }
  return r;
}

// C1: enumerator as a call argument.
static int decode(Color c) {
  return static_cast<int>(c) * 2;
}

// The FR-113 4-line repro (spdlog's Spec/PT shape): an out-of-range value
// through the narrow u8 underlying type into a struct field.
static int probe(int argc) {
  Spec s = Spec();
  s.pt = static_cast<PT>(argc + 300);
  return (int)s.pt + s.width;
}

// C3: cast-to-enum inside a class method must see the module-level
// enum_def through the emitrust.impl symbol-table boundary.
class Widget {
public:
  int n;
  int shade() {
    Color c = static_cast<Color>(n % 3);
    int r = 0;
    if (c == Color::Green) {
      r = 5;
    }
    return r + static_cast<int>(c);
  }
};

int main(int argc, char **argv) {
  // C1 direct init: the most common scoped idiom.
  Color c = Color::Green;
  printf("init %d\n", static_cast<int>(c));

  // static_cast INTO the enum from an argc-derived value; == and !=,
  // including an enumerator compared against a cast.
  Color d = static_cast<Color>(argc);
  if (d == Color::Green) printf("green\n");
  if (d != Color::Red) printf("notred\n");
  if (static_cast<Color>(argc) == Color::Green) printf("eq\n");

  // switch with scoped case labels, both an in-range and an out-of-range
  // (default-taking) selector.
  switch (d) {
  case Color::Red: printf("r\n"); break;
  case Color::Green: printf("g\n"); break;
  default: printf("d\n"); break;
  }
  switch (static_cast<Color>(argc + 6)) {
  case Color::Blue: printf("blue\n"); break;
  case Color::Red: printf("red\n"); break;
  default: printf("other\n"); break;
  }

  // Out-of-range through the u8 underlying type: C++ truncates 301 -> 45.
  PT p = static_cast<PT>(argc + 300);
  printf("trunc %d\n", (int)p);

  // Enum field; comparison against an enumerator.
  Spec s = Spec();
  s.pt = static_cast<PT>(argc + 44);
  s.width = static_cast<int>(s.pt) + argc;
  printf("spec %d %d\n", (int)(s.pt == PT::B), s.width);

  // Enum param/return plus an enumerator argument.
  Color r = pick(d, argc + 1);
  printf("pick %d\n", static_cast<int>(r));
  printf("arg %d\n", decode(Color::Blue));

  // The Spec/PT repro leg.
  printf("probe %d\n", probe(argc));

  // C3 leg: the enum cast inside a method.
  Widget w;
  w.n = argc + 4;
  printf("widget %d\n", w.shade());
  return 0;
}
