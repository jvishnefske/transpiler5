// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/vector-bad-element.cpp 2>&1 | FileCheck %s --check-prefix=BADELEM
// RUN: not emitrust-import-c %t/unsupported-entity-map.cpp 2>&1 | FileCheck %s --check-prefix=BADMAP
// RUN: not emitrust-import-c %t/unsupported-entity-clog.cpp 2>&1 | FileCheck %s --check-prefix=BADCLOG
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
// RUN: not emitrust-import-c %t/lambda-mutable.cpp 2>&1 | FileCheck %s --check-prefix=LAMMUT
// RUN: not emitrust-import-c %t/lambda-ref-capture.cpp 2>&1 | FileCheck %s --check-prefix=LAMREFCAP
// RUN: not emitrust-import-c %t/lambda-default-copy.cpp 2>&1 | FileCheck %s --check-prefix=LAMDEFCOPY
// RUN: not emitrust-import-c %t/lambda-default-ref.cpp 2>&1 | FileCheck %s --check-prefix=LAMDEFREF
// RUN: not emitrust-import-c %t/lambda-nonscalar-capture.cpp 2>&1 | FileCheck %s --check-prefix=LAMNONSCALAR
// RUN: not emitrust-import-c %t/lambda-escape-copy.cpp 2>&1 | FileCheck %s --check-prefix=LAMESCCOPY
// RUN: not emitrust-import-c %t/lambda-escape-arg.cpp 2>&1 | FileCheck %s --check-prefix=LAMESCARG
// RUN: not emitrust-import-c %t/variant-three.cpp 2>&1 | FileCheck %s --check-prefix=VARTHREE
// RUN: not emitrust-import-c %t/variant-duplicate.cpp 2>&1 | FileCheck %s --check-prefix=VARDUP
// RUN: not emitrust-import-c %t/variant-nonscalar.cpp 2>&1 | FileCheck %s --check-prefix=VARNONSCALAR
// RUN: not emitrust-import-c %t/variant-holds.cpp 2>&1 | FileCheck %s --check-prefix=VARHOLDS
// RUN: not emitrust-import-c %t/variant-visit.cpp 2>&1 | FileCheck %s --check-prefix=VARVISIT
// RUN: not emitrust-import-c %t/variant-get-index.cpp 2>&1 | FileCheck %s --check-prefix=VARGETIDX
// RUN: not emitrust-import-c %t/variant-valueless.cpp 2>&1 | FileCheck %s --check-prefix=VARVALUELESS

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

//--- unsupported-entity-clog.cpp
#include <iostream>
// A std::ostream global is a VarDecl the importer never imports (unused
// system-header decl, per the existing SKIP); a reference to it hits the
// pre-existing system-header-reference rejection BEFORE `mapType` (and
// thus `mapStdLibraryType`) ever runs on its type.
//
// W2.22 MOVED this pin off `std::cout`: cout/cerr `<<` chains are now
// admitted (see ostream-print.cpp), so the section used to screen a
// rejection that no longer happens. `std::clog` is the right screen now --
// W2.22's recognizer keys on the base VarDecl's NAME being exactly
// `cout`/`cerr`, precisely so every other stream (clog, cin, wcout, any
// user-declared ostream) keeps this diagnostic instead of being admitted
// on an "is it an ostream?" type test.
// BADCLOG: unsupported-entity-clog.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference to 'clog' declared in a system header; not part of the supported C subset
int use(void) {
  std::clog << 1;
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


//--- lambda-mutable.cpp
// W2.13 frontier: the lambda lift freezes each by-value capture ONCE at
// the declaration point; a `mutable` lambda's operator() can write its
// closure copy, state the lift has no representation for.
// LAMMUT: lambda-mutable.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutable lambda
int use(void) {
  int a = 1;
  auto f = [a](int x) mutable { return a += x; };
  return f(1);
}

//--- lambda-ref-capture.cpp
// W2.13 frontier: an explicit by-reference capture aliases the enclosing
// local — the opposite of the freeze-at-declaration model.
// LAMREFCAP: lambda-ref-capture.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: lambda capture by reference
int use(void) {
  int a = 1;
  auto f = [&a](int x) { return a + x; };
  return f(1);
}

//--- lambda-default-copy.cpp
// W2.13 frontier: a default capture ([=] or [&]) captures an implicit,
// use-derived set; only an EXPLICIT by-value capture list is recognized.
// LAMDEFCOPY: lambda-default-copy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: lambda default capture; only explicit by-value captures are supported
int use(void) {
  int a = 1;
  auto f = [=](int x) { return a + x; };
  return f(1);
}

//--- lambda-default-ref.cpp
// W2.13 frontier: the by-reference default capture shares the
// default-capture rejection above.
// LAMDEFREF: lambda-default-ref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: lambda default capture; only explicit by-value captures are supported
int use(void) {
  int a = 1;
  auto f = [&](int x) { return a + x; };
  return f(1);
}

//--- lambda-nonscalar-capture.cpp
// W2.13 frontier: only SCALAR (integer/floating) captures freeze to a
// prepended parameter; an aggregate capture would need a by-value struct
// copy at the declaration point.
// LAMNONSCALAR: lambda-nonscalar-capture.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: lambda capture of a non-scalar variable
struct Pt { int x; int y; };
int use(void) {
  Pt p = {1, 2};
  auto f = [p]() { return p.x + p.y; };
  return f();
}

//--- lambda-escape-copy.cpp
// W2.13 frontier: every use of the lambda local must be a direct
// operator() call; copying it into another variable lets the closure
// escape the lift's call-rewrite, so the recognizer rejects AT THE USE.
// LAMESCCOPY: lambda-escape-copy.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: lambda 'f' escapes its declaration (every use must be a direct call)
int use(void) {
  int a = 1;
  auto f = [a](int x) { return a + x; };
  auto g = f;
  return g(1);
}

//--- lambda-escape-arg.cpp
// W2.13 frontier: passing a lambda to a function requires naming its
// closure type, which in practice means a function template.
//
// W2.15 MOVED this pin forward rather than loosening it. Until function
// templates were monomorphized, `apply` was rejected wholesale as
// `unsupported top-level declaration` and the lambda never mattered.
// Now clang's `apply<(lambda ...)>` instantiation IS imported, so the
// rejection lands one level deeper and one level more precisely: on the
// closure class's `operator()`, at the lambda's own source location. The
// escape-by-argument shape is still out — that is the invariant this
// section pins — but the diagnostic now names the real obstacle (the
// closure type is a record with an overloaded call operator) instead of
// the incidental one.
// LAMESCARG: lambda-escape-arg.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: overloaded operator
template <typename F> int apply(F f) { return f(1); }
int use(void) {
  int a = 1;
  auto f = [a](int x) { return a + x; };
  return apply(f);
}

//--- variant-three.cpp
#include <variant>
// W2.14 frontier: only a TWO-alternative std::variant synthesizes the
// closed data enum (the match expansions and the V0/V1 image are
// two-arm by construction); any other arity is a located type-level
// rejection.
// VARTHREE: variant-three.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: only a two-alternative std::variant is recognized
int use(void) {
  std::variant<int, double, char> t;
  return 0;
}

//--- variant-duplicate.cpp
#include <variant>
// W2.14 frontier: duplicate alternatives are indistinguishable BY TYPE
// at every use site (the converting ctor, operator=, and std::get<T>
// all select the variant by exact mapped-type equality), so the shape
// stays a type-level rejection. NOTE the default-ctor spelling: the
// `= 1` form is ill-formed C++ (ambiguous converting ctor) and dies in
// the clang frontend before the importer runs.
// VARDUP: variant-duplicate.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::variant with duplicate alternatives
int use(void) {
  std::variant<int, int> d;
  return 0;
}

//--- variant-nonscalar.cpp
#include <string>
#include <variant>
// W2.14 frontier: only scalar (signed-integer/floating) alternatives
// are in this wave's set — a std::string alternative maps to an owned
// String, which the Copy-deriving data_enum_def payload field set
// cannot hold.
// VARNONSCALAR: variant-nonscalar.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::variant alternative type is not in the supported scalar set
int use(void) {
  std::variant<int, std::string> s;
  return 0;
}

//--- variant-holds.cpp
#include <variant>
// W2.14 frontier: holds_alternative is OUT this wave (the corpus only
// gets the held alternative; no alternative-state tracker exists in the
// importer) — a located FREE-function rejection, since std::variant's
// vocabulary lives in free functions, not methods.
// VARHOLDS: variant-holds.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::holds_alternative is not a recognized STL function
int use(void) {
  std::variant<int, double> v = 1;
  bool h = std::holds_alternative<int>(v);
  return (int)h;
}

//--- variant-visit.cpp
#include <variant>
// W2.14 frontier: std::visit's callable dispatch has no image (the
// visitor is a generic lambda — a template the importer never admits);
// rejected AT THE CALL, before the visitor is ever imported.
// VARVISIT: variant-visit.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::visit is not a recognized STL function
int use(void) {
  std::variant<int, double> v = 1;
  std::visit([](auto) {}, v);
  return 0;
}

//--- variant-get-index.cpp
#include <variant>
// W2.14 frontier: only the alternative-TYPE form std::get<T> is
// recognized (the type selects the held arm of the match expansion);
// the index form get<0> would need the same selection routed through
// an integral template argument — a located rejection this wave.
// VARGETIDX: variant-get-index.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::get<index> over a std::variant (only the alternative-type form std::get<T> is recognized)
int use(void) {
  std::variant<int, double> v = 1;
  int g = std::get<0>(v);
  return g;
}

//--- variant-valueless.cpp
#include <variant>
// W2.14 frontier: valueless_by_exception is exception-machinery state
// the importer's exception-free subset can never reach; it falls to the
// per-class STL-method rejection naming the entity.
// VARVALUELESS: variant-valueless.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::variant::valueless_by_exception is not a recognized STL method
int use(void) {
  std::variant<int, double> v = 1;
  bool b = v.valueless_by_exception();
  return (int)b;
}
