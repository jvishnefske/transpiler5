// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/value-default.cpp 2>&1 | FileCheck %s --check-prefix=VALDEFAULT

// The whole-struct value copy admitted for a by-value struct argument/return
// is gated strictly on a TRIVIAL copy/move constructor with a single source
// operand. A value-position construction with no such source — here a default
// construction `Point()` passed by value — must reject, located, rather than
// silently materializing a zeroed temporary (which could mask a real, missing
// constructor side effect). This pins the miscompile guard.

//--- value-default.cpp
struct Point {
  int x;
  int y;
};

int sum(Point p) {
  return p.x + p.y;
}

// VALDEFAULT: value-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor in value position (only a trivial copy or move is modeled)
int use() {
  return sum(Point());
}
