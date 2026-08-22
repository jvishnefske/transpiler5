// RUN: emitrust-import-c %s | FileCheck %s
// W2.22: a statement-position `std::cout` / `std::cerr` `<<` chain lowers
// to the Rust print macros. Pins the four decisions the byte-diff oracle
// depends on, at the IR level, so a later "cleanup" cannot quietly move
// them:
//
//  1. FUSION. A chain is fused into ONE `println!`/`print!` with a
//     multi-slot format string, not one macro call per operand. String
//     literals fold into the format text (`{`/`}` doubled), so
//     `cout << "i=" << i << std::endl` is a single `println!("i={}", i)`.
//     `std::endl` contributes only a newline byte, which `emitPrintMacro`
//     folds into the `println!` spelling.
//
//  2. SEGMENTATION, which is what keeps fusion CORRECT. C++17 sequences
//     `E1 << E2` left to right and writes E1's output BEFORE E2 is
//     evaluated; hoisting every operand into a `let` ahead of one macro
//     call reorders output whenever an operand's side effect writes to a
//     stream (measured: `<X1>a2b` where C++ prints `a<X1>2b`). So the
//     pending segment is FLUSHED before a side-effecting operand is
//     evaluated. A `char` operand flushes for the same reason -- it is a
//     raw byte write, not a format hole.
//
//  3. THE FORMATTING FUNNELS, each chosen against a measured libstdc++
//     divergence: `double`/`float` through `__emitrust_fmt_float(.., 2, -1,
//     0, 0)` (libstdc++ `operator<<(double)` is `%g` with 6 significant
//     digits, where Rust's `{}` prints `0.3333333333333333` and `1000000`);
//     `bool` zero-extended to i32 (C++ prints `1`/`0`, Rust's `{}` prints
//     `true`/`false`); `char`/`signed char`/`unsigned char` through
//     `__emitrust_byte_out` (all three are CHARACTER overloads writing ONE
//     raw byte, where the `__emitrust_fmt_c` Display funnel emits two-byte
//     UTF-8 for 128..=255); every integer through a cast to the RESOLVED
//     overload's exact width and signedness.
//
//  4. `std::cerr` selects the stderr twins `eprint!`/`eprintln!` and
//     `__emitrust_byte_err` -- no new dialect op, `emitrust.call_opaque`
//     already renders any callee verbatim.
//
// The byte-for-byte oracle for all of it is test/EndToEnd/cpp-iostream.cpp
// and the Cpp17Suite corpus entry 01005.cpp; this file pins the SHAPE so a
// regression names itself here first. See test/Import/Cpp/ostream-invalid.cpp
// for everything that stays a located rejection.
#include <iostream>
#include <string>

extern "C" int printf(const char *, ...);

static int trace = 0;
int bump(int k) {
  trace = trace * 10 + k;
  printf("<b%d>", k);
  return trace;
}

// CHECK-LABEL: func.func @literal_only
void literal_only(void) {
  // A chain of literals only: the whole text folds into the format string,
  // braces doubled, and the trailing endl picks `println!`.
  // CHECK: emitrust.call_opaque "println!"() {args = ["a{{[{][{][}][}]}}b"]}
  std::cout << "a{" << "}b" << std::endl;
}

// CHECK-LABEL: func.func @bare_endl
void bare_endl(void) {
  // The whole format is one newline with no holes: rendered from an empty
  // args array so the emitter spells `println!()`, never `println!("")`.
  // CHECK: emitrust.call_opaque "println!"() {args = []}
  std::cout << std::endl;
}

// CHECK-LABEL: func.func @no_newline_tail
void no_newline_tail(int i) {
  // No endl anywhere: `print!`, with no trailing newline invented.
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}) {args = ["tail:{}", 0 : index]}
  std::cout << "tail:" << i;
}

// CHECK-LABEL: func.func @integers
void integers(short sh, unsigned short ush, int i, unsigned u, long l,
              unsigned long ul) {
  // Each operand is cast to the RESOLVED overload's width and signedness
  // (this is why an unscoped enum or a size_t prints what C++ prints, not
  // what the written type suggests), then printed with a plain `{}` -- Rust
  // and libstdc++ agree byte for byte on every integer width, at the
  // extremes included.
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["{} {} {} {} {} {}", 0 : index, 1 : index, 2 : index, 3 : index, 4 : index, 5 : index]} : (i16, ui16, i32, ui32, i64, ui64) -> ()
  std::cout << sh << " " << ush << " " << i << " " << u << " " << l << " "
            << ul << std::endl;
}

// CHECK-LABEL: func.func @booleans
void booleans(int i) {
  bool flag = i > 0;
  // C++ prints 1/0 in the default (non-boolalpha) stream state; the
  // zero-extension to i32 is what makes Rust's `{}` agree.
  // CHECK: %[[B:.*]] = arith.extui %{{.*}} : i1 to i32
  // CHECK: emitrust.call_opaque "println!"(%[[B]]) {args = ["{}", 0 : index]}
  std::cout << flag << std::endl;
}

// CHECK-LABEL: func.func @floats
void floats(double d, float f) {
  // Both overloads are %g/6-significant-digits by construction in
  // libstdc++, which the project's C-compatible helper reproduces exactly
  // (conv=2 is 'g', prec=-1 is C's default 6). The float widens to f64
  // first, exactly as `operator<<(float)` hands its argument to
  // `_M_insert<double>`.
  // CHECK: %[[D:.*]] = emitrust.call_opaque "__emitrust_fmt_float"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">
  // CHECK: %[[W:.*]] = arith.extf %{{.*}} : f32 to f64
  // CHECK: %[[F:.*]] = emitrust.call_opaque "__emitrust_fmt_float"(%[[W]], %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) : (f64, i32, i32, i32, i32) -> !emitrust.opaque<"String">
  // CHECK: emitrust.call_opaque "println!"(%[[D]], %[[F]]) {args = ["{} {}", 0 : index, 1 : index]}
  std::cout << d << " " << f << std::endl;
}

// CHECK-LABEL: func.func @chars
void chars(char c, signed char sc, unsigned char uc) {
  // All three are CHARACTER overloads in libstdc++ (measured: `(signed
  // char)-5` and `(unsigned char)200` each write ONE raw byte, not a
  // number). A raw write is not a format hole, so the pending segment
  // flushes around each one, in program order, on the same buffered
  // stdout handle `print!` locks.
  // CHECK: emitrust.call_opaque "print!"() {args = ["c="]}
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"(%{{.*}}) : (i8) -> ()
  // CHECK: emitrust.call_opaque "print!"() {args = [" sc="]}
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"(%{{.*}}) : (i8) -> ()
  // CHECK: emitrust.call_opaque "print!"() {args = [" uc="]}
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"(%{{.*}}) : (i8) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = []}
  std::cout << "c=" << c << " sc=" << sc << " uc=" << uc << std::endl;
}

// CHECK-LABEL: func.func @strings
void strings(void) {
  std::string s = "ab";
  // A std::string operand borrows its String place shared and prints by
  // Display -- the same lowering, on the same place, that
  // `printf("%s", s.c_str())` already uses.
  // CHECK: %[[R:.*]] = emitrust.addr_of %{{.*}} : (!emitrust.lvalue<!emitrust.opaque<"String">>) -> !emitrust.ref<!emitrust.opaque<"String">>
  // CHECK: emitrust.call_opaque "println!"(%[[R]]) {args = ["s={}", 0 : index]}
  std::cout << "s=" << s << std::endl;
}

// CHECK-LABEL: func.func @side_effecting_operands
void side_effecting_operands(void) {
  // THE sequencing pin. `bump()` writes to stdout itself, so the pending
  // segment MUST be flushed before each call: the emitted order is
  // print!("seq:") / bump(1) / print!("{},") / bump(2) / ... Fusing the
  // three calls ahead of one macro would print `<b1><b2><b3>1,12,123`
  // where C++ prints `<b1>1,<b2>12,<b3>123`.
  // CHECK: emitrust.call_opaque "print!"() {args = ["seq:"]}
  // CHECK: %[[B1:.*]] = call @bump
  // CHECK: emitrust.call_opaque "print!"(%[[B1]]) {args = ["{},", 0 : index]}
  // CHECK: %[[B2:.*]] = call @bump
  // CHECK: emitrust.call_opaque "print!"(%[[B2]]) {args = ["{},", 0 : index]}
  // CHECK: %[[B3:.*]] = call @bump
  // CHECK: emitrust.call_opaque "println!"(%[[B3]]) {args = ["{}", 0 : index]}
  std::cout << "seq:" << bump(1) << "," << bump(2) << "," << bump(3)
            << std::endl;
}

// CHECK-LABEL: func.func @errors
void errors(int i, char c) {
  // std::cerr selects the stderr twins throughout -- the macro AND the raw
  // byte funnel. `emitrust.call_opaque` renders any callee verbatim, so no
  // dialect op or emitter change is involved.
  // CHECK: emitrust.call_opaque "eprintln!"(%{{.*}}) {args = ["err {}", 0 : index]}
  std::cerr << "err " << i << std::endl;
  // CHECK: emitrust.call_opaque "eprint!"() {args = ["b="]}
  // CHECK: emitrust.call_opaque "__emitrust_byte_err"(%{{.*}}) : (i8) -> ()
  // CHECK: emitrust.call_opaque "eprintln!"() {args = []}
  std::cerr << "b=" << c << std::endl;
}
