// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/vector-bad-element.cpp 2>&1 | FileCheck %s --check-prefix=BADELEM
// RUN: not emitrust-import-c %t/unsupported-entity-map.cpp 2>&1 | FileCheck %s --check-prefix=BADMAP
// RUN: not emitrust-import-c %t/unsupported-entity-cout.cpp 2>&1 | FileCheck %s --check-prefix=BADCOUT
// RUN: not emitrust-import-c %t/bad-string-char.cpp 2>&1 | FileCheck %s --check-prefix=BADWCHAR
// RUN: not emitrust-import-c %t/bad-vector-method.cpp 2>&1 | FileCheck %s --check-prefix=BADMETHOD
// RUN: not emitrust-import-c %t/bad-string-method.cpp 2>&1 | FileCheck %s --check-prefix=BADSTRMETHOD
// RUN: not emitrust-import-c %t/sized-ctor.cpp 2>&1 | FileCheck %s --check-prefix=SIZEDCTOR
// RUN: not emitrust-import-c %t/initlist-ctor.cpp 2>&1 | FileCheck %s --check-prefix=INITLISTCTOR
// RUN: not emitrust-import-c %t/copy-ctor.cpp 2>&1 | FileCheck %s --check-prefix=COPYCTOR
// RUN: not emitrust-import-c %t/string-index.cpp 2>&1 | FileCheck %s --check-prefix=STRINGINDEX
// RUN: not emitrust-import-c %t/cstr-elsewhere.cpp 2>&1 | FileCheck %s --check-prefix=CSTRELSEWHERE
// RUN: not emitrust-import-c %t/ranged-for.cpp 2>&1 | FileCheck %s --check-prefix=RANGEDFOR

// W2.3 located rejections: every construct explicitly OUT of the STL
// recognition surface this wave, authored so a later wave has a documented
// baseline (mirroring cpp-basics-invalid.cpp's W2.0 baseline). See
// design.md's STL section for the full OUT list and rationale.

//--- vector-bad-element.cpp
#include <vector>
// An element type outside the supported scalar/struct/nested-STL set (a
// raw pointer) is rejected by the RECURSIVE mapType call on T — propagated
// as-is, not re-worded, exactly like every other nested mapType failure in
// this importer (e.g. a struct field of unsupported type).
// BADELEM: vector-bad-element.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer type outside a parameter position
int use(void) {
  std::vector<int *> v;
  return 0;
}

//--- unsupported-entity-map.cpp
#include <map>
// BADMAP: unsupported-entity-map.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map is not a recognized STL type
int use(void) {
  std::map<int, int> m;
  return 0;
}

//--- unsupported-entity-cout.cpp
#include <iostream>
// std::cout is a global VarDecl the importer never imports (unused
// system-header decl, per the existing SKIP); a reference to it hits the
// pre-existing system-header-reference rejection BEFORE `mapType` (and
// thus `mapStdLibraryType`) ever runs on its type.
// BADCOUT: unsupported-entity-cout.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference to 'cout' declared in a system header; not part of the supported C subset
int use(void) {
  std::cout << 1;
  return 0;
}

//--- bad-string-char.cpp
#include <string>
// BADWCHAR: bad-string-char.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_string with a non-char character type
int use(void) {
  std::wstring s;
  return 0;
}

//--- bad-vector-method.cpp
#include <vector>
// Only push_back/size/operator[]/at/empty/clear (W2.3) plus
// front/back/pop_back (W2.6) are recognized; every other vector method
// (insert, erase, resize, reserve, begin/end, ...) is a located rejection
// naming the entity. (This split used pop_back until W2.6 landed it.)
// BADMETHOD: bad-vector-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::vector::resize is not a recognized STL method
int use(void) {
  std::vector<int> v;
  v.resize(3);
  return 0;
}

//--- bad-string-method.cpp
#include <string>
// BADSTRMETHOD: bad-string-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string::find is not a recognized STL method
int use(void) {
  std::string s;
  s.find('a');
  return 0;
}

//--- sized-ctor.cpp
#include <vector>
// The sized/fill vector constructor (`std::vector<T>(n)`) is OUT this
// wave: it would need a templated-literal or `vec!`-shaped op this wave's
// op vocabulary (emitrust.call_opaque's plain `callee(args)` shape,
// emitrust.method_call's `place.method(args)` shape) cannot spell.
// SIZEDCTOR: sized-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: this std::vector/std::string constructor shape is not supported (only default construction and std::string's string-literal conversion constructor are recognized)
int use(void) {
  std::vector<int> v(5);
  return 0;
}

//--- initlist-ctor.cpp
#include <vector>
// An initializer-list constructor is likewise OUT this wave.
// INITLISTCTOR: initlist-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: this std::vector/std::string constructor shape is not supported (only default construction and std::string's string-literal conversion constructor are recognized)
int use(void) {
  std::vector<int> v{1, 2, 3};
  return 0;
}

//--- copy-ctor.cpp
#include <vector>
// Copy construction is a located rejection (a distinct, sharper message
// than the generic constructor-shape rejection above).
// COPYCTOR: copy-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::vector/std::string copy/move construction
int use(void) {
  std::vector<int> v;
  std::vector<int> w = v;
  return 0;
}

//--- string-index.cpp
#include <string>
// std::string::operator[] (bytes indexing) is OUT this wave: Rust's
// `String` has no `Index<usize>` impl (only `[T]`/`Vec<T>` do), so it
// needs a distinct "index into as_bytes()" spelling this wave does not
// implement.
// STRINGINDEX: string-index.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string::operator[] is not a recognized STL method (bytes indexing is not supported this wave)
int use(void) {
  std::string s = "hi";
  return s[0];
}

//--- cstr-elsewhere.cpp
#include <string>
// c_str() is recognized ONLY as a direct printf '%s' argument; anywhere
// else (here, a plain call whose result is discarded) it is a located
// rejection.
// CSTRELSEWHERE: cstr-elsewhere.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string::c_str() is only recognized as a printf '%s' argument
int use(void) {
  std::string s = "hi";
  s.c_str();
  return 0;
}

//--- ranged-for.cpp
#include <vector>
// Ranged-for over a vector is OUT this wave: `CXXForRangeStmt` has no
// statement-import case at all (deferred, not merely unrecognized-method
// rejected), so it falls through the EXISTING generic statement dispatch
// exactly like W2.0's try/catch baseline (cpp-basics-invalid.cpp).
// RANGEDFOR: ranged-for.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported statement: CXXForRangeStmt
int use(void) {
  std::vector<int> v;
  int total = 0;
  for (int x : v) {
    total += x;
  }
  return total;
}

