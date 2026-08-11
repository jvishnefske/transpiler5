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
// RUN: not emitrust-import-c %t/optional-copy-ctor.cpp 2>&1 | FileCheck %s --check-prefix=OPTCOPY
// RUN: not emitrust-import-c %t/optional-value.cpp 2>&1 | FileCheck %s --check-prefix=OPTVALUE
// RUN: not emitrust-import-c %t/optional-deref.cpp 2>&1 | FileCheck %s --check-prefix=OPTDEREF
// RUN: not emitrust-import-c %t/string-view-param.cpp 2>&1 | FileCheck %s --check-prefix=SVPARAM
// RUN: not emitrust-import-c %t/string-view-from-string.cpp 2>&1 | FileCheck %s --check-prefix=SVFROMSTRING
// RUN: not emitrust-import-c %t/string-view-rebind.cpp 2>&1 | FileCheck %s --check-prefix=SVREBIND
// RUN: not emitrust-import-c %t/string-view-substr.cpp 2>&1 | FileCheck %s --check-prefix=SVSUBSTR

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
// W2.10 FLIPPED this pin: ranged-for over a container local now imports
// (test/Import/Cpp/stl-ranged-for.cpp). The frontier moved to the R3
// safety restriction: the loop BODY may not name the range variable at
// all — that is the guarantee that the container's length is
// loop-invariant, which is what makes the len()-per-iteration desugar
// exact against C++'s evaluate-end-once semantics (a push_back inside
// the body would diverge silently otherwise).
// RANGEDFOR: ranged-for.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: ranged-for body may not use the range variable
int use(void) {
  std::vector<int> v;
  int total = 0;
  for (int x : v) {
    v.push_back(x);
    total += x;
  }
  return total;
}

//--- optional-copy-ctor.cpp
#include <optional>
// W2.11 frontier: copy construction of a std::optional is OUT (the same
// copy/move guard that protects vector/string construction fires first
// in emitStlConstruct, so the message keeps its established spelling).
// OPTCOPY: optional-copy-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::vector/std::string copy/move construction
int use(void) {
  std::optional<int> a;
  std::optional<int> b = a;
  return 0;
}

//--- optional-value.cpp
#include <optional>
// W2.11 frontier: only has_value()/value_or() are recognized; value()
// is OUT (Rust's unwrap panic message differs from the C++ exception,
// so it cannot be silently substituted).
// (A VALUE-READ of value() — `return a.value();` — is caught even
// earlier, by the generic assignable-expression rejection in emitLValue,
// since value() returns `T&`; the statement shape below reaches the
// optional method table and pins its sharper, entity-naming wording.)
// OPTVALUE: optional-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::optional::value is not a recognized STL method
int use(void) {
  std::optional<int> a;
  a.value();
  return 0;
}

//--- optional-deref.cpp
#include <optional>
// W2.11 frontier: operator* is OUT (UB when empty; no place model for
// the contained value this wave) — it reaches emitStlOperatorCall's
// operator table, whose default arm rejects it.
// OPTDEREF: optional-deref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: this STL operator is not a recognized STL method
int use(void) {
  std::optional<int> a;
  *a;
  return 0;
}

//--- string-view-param.cpp
#include <string_view>
// W2.12 frontier: only a literal-initialized string_view LOCAL is
// decomposed (into backing + cursor/len cells; no string_view type is
// ever materialized). A parameter (or return) still maps its type and
// keeps the mapType-tail rejection — the decomposition never applies.
// SVPARAM: string-view-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_string_view is not a recognized STL type
int take(std::string_view p) { return (int)p.size(); }

//--- string-view-from-string.cpp
#include <string>
#include <string_view>
// W2.12 frontier: constructing a string_view over a std::string (a
// UserDefinedConversion member call, not the recognized literal ctor
// chain) falls through the local interception to the same mapType-tail
// rejection — the view would dangle-track a heap buffer the cursor/len
// decomposition cannot model.
// SVFROMSTRING: string-view-from-string.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_string_view is not a recognized STL type
int use(void) {
  std::string s = "hi";
  std::string_view sv = s;
  return 0;
}

//--- string-view-rebind.cpp
#include <string_view>
// W2.12 frontier: rebinding after init (operator=) is OUT — the
// decomposition binds ONE literal backing for the local's lifetime, and
// a rebind would need to re-point the shared backing.
// SVREBIND: string-view-rebind.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string_view::operator= is not a recognized STL method
int use(void) {
  std::string_view sv = "abc";
  sv = "other";
  return 0;
}

//--- string-view-substr.cpp
#include <string_view>
// W2.12 frontier: only size()/remove_prefix()/operator[] are recognized;
// every other method (data, substr, find, remove_suffix, front, back,
// comparisons) is a located rejection naming the entity.
// SVSUBSTR: string-view-substr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::string_view::substr is not a recognized STL method
int use(void) {
  std::string_view sv = "abc";
  sv.substr(1);
  return 0;
}

