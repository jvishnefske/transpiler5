// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/enum-class.cpp 2>&1 | FileCheck %s --check-prefix=ENUMCLASS
// RUN: not emitrust-import-c %t/enum-struct.cpp 2>&1 | FileCheck %s --check-prefix=ENUMSTRUCT
// RUN: not emitrust-import-c %t/nonliftable-default.cpp 2>&1 | FileCheck %s --check-prefix=NLDEFAULT

// Two Stage-D diagnostic-quality fixes:
//  - a scoped enumeration (`enum class`/`enum struct`) rejects with a specific
//    message located AT THE DEFINITION, not the old misleading "assigned value
//    type does not match the place" surfacing at the first use site;
//  - a defaulted argument the importer cannot lift now rejects with a real
//    file:line:col location (recursed into the default value expression),
//    where it used to reject WITHOUT a location prefix.

//--- enum-class.cpp
// ENUMCLASS: enum-class.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: scoped enumeration (enum class/struct)
enum class Color { Red, Green, Blue };
int use() {
  Color c = Color::Green;
  return (int)c;
}

//--- enum-struct.cpp
// ENUMSTRUCT: enum-struct.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: scoped enumeration (enum class/struct)
enum struct Mode { Off, On };
int use() {
  Mode m = Mode::On;
  return (int)m;
}

//--- nonliftable-default.cpp
struct Widget {
  int x;
};
// The default value is a value-position construction, which does not lift; the
// rejection must now carry the default expression's own location.
// NLDEFAULT: nonliftable-default.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: constructor in value position (only a trivial copy or move is modeled)
int take(int a, Widget w = Widget()) {
  return a + w.x;
}
int use() {
  return take(5);
}
