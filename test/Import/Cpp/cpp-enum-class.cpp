// RUN: emitrust-import-c %s | FileCheck %s
// RUN: emitrust-cc --emit=rust %s | FileCheck %s --check-prefix=RUST

// FR-113: a scoped enumeration imports as its underlying-typed C-like image
// -- the SAME open-enum emitrust.enum_def an unscoped enum gets (scoping is
// compile-time namespacing that clang already resolved at every use site).
// Three companion behaviors are pinned with it:
//  - C1: `Color c = Color::Green;` -- in C++ the enumerator reference HAS
//    the enum type (unlike C, where it is an int), so it imports as the
//    enum-typed constant assigned directly, not through the old
//    castEnumToI32 path that made the assignment reject with a misleading
//    "assigned value type does not match the place";
//  - C2: enum_def storage is fixed u32/i32, but a fixed NARROW underlying
//    type truncates in C++, so an integer-to-enum conversion first casts to
//    the underlying width: `Pt((v + 300i32) as u8 as u32)`;
//  - C3: a cast-to-enum inside a class METHOD renders -- emitrust.impl is a
//    SymbolTable, and the emitter resolves enum_defs through
//    EnumDefOp::lookupFrom in the enclosing module's table (FR-84 pattern).

enum class Color { Red, Green, Blue = 7 };
enum class PT : unsigned char { A = 10, B = 45 };

int direct() {
  Color c = Color::Green;
  return static_cast<int>(c);
}

PT narrow(int v) {
  PT p = PT::A;
  p = static_cast<PT>(v + 300);
  return p;
}

class Holder {
public:
  int n;
  int method() {
    Color c = static_cast<Color>(n);
    return static_cast<int>(c);
  }
};

// The scoped definitions become ordinary module-level open enums. Color has
// no fixed underlying type, so its underlying is (signed) int; PT's fixed
// `unsigned char` records the unsigned marker.
// CHECK-DAG: emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 7]{{$}}
// CHECK-DAG: emitrust.enum_def @PT ["A", "B"] [10, 45] {unsigned_underlying}

// C1: the enumerator reference is the enum-typed constant, assigned
// directly to the enum-typed place.
// CHECK-LABEL: func.func @direct
// CHECK: %[[C:.*]] = emitrust.variable named "c" : !emitrust.lvalue<!emitrust.enum<"Color">>
// CHECK: %[[G:.*]] = emitrust.constant <#emitrust.opaque<"Color::Green">> : !emitrust.enum<"Color">
// CHECK: emitrust.assign %[[C]] = %[[G]]

// C2: the conversion narrows to the u8 underlying type BEFORE the enum
// construction cast.
// CHECK-LABEL: func.func @narrow
// CHECK: %[[N:.*]] = emitrust.cast %{{[0-9]+}} : i32 to ui8
// CHECK: emitrust.cast %[[N]] : ui8 to !emitrust.enum<"PT">

// RUST-DAG: pub struct Color(pub i32);
// RUST-DAG: pub struct Pt(pub u32);
// C2 rendered: truncate at u8, then the storage constructor.
// RUST: Pt((v + 300i32) as u8 as u32);
// C3: the method's cast-to-enum resolves the module-level enum_def from
// inside the impl and renders the constructor form.
// RUST: impl Holder {
// RUST: c = Color(self.n);
