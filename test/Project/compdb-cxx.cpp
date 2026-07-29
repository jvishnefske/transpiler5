// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/compile_commands.json.in > %t/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t -o - | FileCheck %s
// RUN: not emitrust-cc --emit=import %t/widget.c -o - 2>&1 | FileCheck %s --check-prefix=NODB

// FR-45: language selection comes from the database ENTRY, not from the
// file's extension. The single source is deliberately named `widget.c` —
// an extension `isCxxSourcePath` classifies as C — while its entry
// records `-x c++ -std=c++17`, and the file's contents (a `namespace`,
// `bool`, `true`) are valid C++ and invalid C. The first run therefore
// only succeeds if the extension guess was genuinely bypassed.
//
// The NODB run is the proof of the bypass rather than an accident: the
// same file with no database takes the extension guess, parses as C11, and
// fails on the very first line.
//
// `gadget.c` pins the second half of the rule: an entry with NO `-x` at
// all, whose only C++ signal is that its recorded compiler is a C++
// driver. The recorded `argv[0]` is dropped (it names a path that exists
// on no machine, which is the point: the project's own compiler is never
// probed or executed), and exactly one property of it survives — the
// `++` suffix the clang driver reads as "C++ mode" — so this file too
// imports as C++ despite its `.c` extension.
//
// This test lives under a `.cpp` name only because lit discovers tests by
// suffix; the imported sources are the `.c` files inside it.

//--- compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/widget.c",
    "command": "clang++ -x c++ -std=c++17 -c widget.c -o widget.o"
  },
  {
    "directory": "@DIR@",
    "file": "@DIR@/gadget.c",
    "command": "/nonexistent/toolchain/bin/g++ -c gadget.c -o gadget.o"
  }
]

//--- widget.c
namespace gadgets {
int cube(int side);
} // namespace gadgets

namespace shapes {

int square(int side) { return side * side; }

bool is_positive(int x) { return x > 0; }

} // namespace shapes

int main(void) { return shapes::square(3) + gadgets::cube(2); }

//--- gadget.c
namespace gadgets {

int cube(int side) { return side * side * side; }

} // namespace gadgets

// The namespace prefix in the emitted symbol names is the W2.0 C++ import
// path, reachable here only through the database's `-x c++` (widget.c) and
// its C++ driver name (gadget.c).
// CHECK-DAG: func.func @ns_shapes_square
// CHECK-DAG: func.func @ns_shapes_is_positive
// CHECK-DAG: func.func @ns_gadgets_cube
// CHECK-DAG: func.func @c_main

// NODB: widget.c:1:1: error:
