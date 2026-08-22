// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/value-use-good.cpp 2>&1 | FileCheck %s --check-prefix=VALUEUSE
// RUN: not emitrust-import-c %t/cstr-runtime.cpp 2>&1 | FileCheck %s --check-prefix=CSTRRUNTIME
// RUN: not emitrust-import-c %t/pointer-operand.cpp 2>&1 | FileCheck %s --check-prefix=PTROPERAND
// RUN: not emitrust-import-c %t/manipulator-hex.cpp 2>&1 | FileCheck %s --check-prefix=MANIPHEX
// RUN: not emitrust-import-c %t/manipulator-flush.cpp 2>&1 | FileCheck %s --check-prefix=MANIPFLUSH
// RUN: not emitrust-import-c %t/manipulator-setw.cpp 2>&1 | FileCheck %s --check-prefix=MANIPSETW
// RUN: not emitrust-import-c %t/long-double.cpp 2>&1 | FileCheck %s --check-prefix=LONGDOUBLE
// RUN: not emitrust-import-c %t/string-view.cpp 2>&1 | FileCheck %s --check-prefix=STRINGVIEW
// RUN: not emitrust-import-c %t/non-ascii-literal.cpp 2>&1 | FileCheck %s --check-prefix=NONASCII
// RUN: not emitrust-import-c %t/other-stream.cpp 2>&1 | FileCheck %s --check-prefix=OTHERSTREAM
// RUN: not emitrust-import-c %t/ostream-param.cpp 2>&1 | FileCheck %s --check-prefix=OSTREAMPARAM
// RUN: not emitrust-import-c %t/user-operator.cpp 2>&1 | FileCheck %s --check-prefix=USEROP
// RUN: not emitrust-import-c %t/ostream-ref-bind.cpp 2>&1 | FileCheck %s --check-prefix=REFBIND
// RUN: not emitrust-import-c %t/conditional-stream.cpp 2>&1 | FileCheck %s --check-prefix=CONDSTREAM
// RUN: not emitrust-import-c %t/chain-as-condition.cpp 2>&1 | FileCheck %s --check-prefix=CHAINCOND
// RUN: not emitrust-import-c %t/ostringstream.cpp 2>&1 | FileCheck %s --check-prefix=OSTRINGSTREAM
// RUN: not emitrust-import-c %t/using-namespace.cpp 2>&1 | FileCheck %s --check-prefix=USINGNS

// W2.22 located rejections: the frontier around the admitted
// `std::cout`/`std::cerr` `<<` subset (see ostream-print.cpp for what IS
// admitted). Rejection is a feature here in the strongest sense: every
// shape below either has NO byte-faithful Rust image (libstdc++ formatting
// this wave does not reproduce, or an address that is nondeterministic by
// construction) or no representable value at all, and admitting one on a
// guess would be a silent miscompile the byte-diff oracle would only catch
// if a corpus entry happened to exercise it.
//
// Half of these sections are ALREADY-FENCED shapes, kept here on purpose:
// admitting cout/cerr moved a lot of machinery, and each one records the
// exact pre-existing wording that must NOT drift into an accidental
// admission. Their diagnostics are noted per-section.

//--- value-use-good.cpp
#include <iostream>
// The lowering is STATEMENT-POSITION only: `print!` returns nothing, so a
// chain has no ostream value to hand back. Before W2.22 this reported the
// generic "unsupported assignable expression: CXXOperatorCallExpr"; the
// wording now names the real cause, mirroring printf's
// "return value must be unused".
// VALUEUSE: value-use-good.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the result of a std::ostream << chain must be unused
int use(void) {
  int x = (std::cout << 1).good();
  return x;
}

//--- cstr-runtime.cpp
#include <iostream>
// A `const char *` operand folds into the format string only when it is a
// string LITERAL. A runtime pointer would need the raw `__emitrust_cstr_out`
// byte funnel: the Latin-1 `__emitrust_cstr` Display funnel that printf's
// `%s` uses double-encodes a runtime byte >= 128 into two UTF-8 bytes where
// C++ writes one (measured), so this wave rejects rather than inherit that
// channel.
// CSTRRUNTIME: cstr-runtime.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a 'const char *' std::ostream << operand must be a string literal
int use(const char *p) {
  std::cout << p;
  return 0;
}

//--- pointer-operand.cpp
#include <iostream>
// `operator<<(const void *)` prints an ADDRESS. The pointer decomposition
// has compiled provenance away, and no address could be byte-diffed against
// a native run anyway -- the same policy `%p` keeps.
// PTROPERAND: pointer-operand.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a pointer std::ostream << operand prints a nondeterministic address
int use(int a) {
  const void *p = &a;
  std::cout << p;
  return 0;
}

//--- manipulator-hex.cpp
#include <iostream>
// `std::endl` is the ONE manipulator this wave models (a newline; its flush
// is unobservable). A state-changing manipulator would have to be tracked
// across statements to know how later operands format -- no such model
// exists, so admitting it would silently print decimal where C++ prints hex.
// MANIPHEX: manipulator-hex.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::hex is not a recognized std::ostream manipulator
int use(void) {
  std::cout << std::hex << 255;
  return 0;
}

//--- manipulator-flush.cpp
#include <iostream>
// `std::flush` is byte-unobservable in the separate-stream oracle, but the
// abort/panic paths where a missing flush BECOMES observable were never
// measured; the located rejection is the loud-failure direction.
// MANIPFLUSH: manipulator-flush.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::flush is not a recognized std::ostream manipulator
int use(void) {
  std::cout << 1 << std::flush;
  return 0;
}

//--- manipulator-setw.cpp
#include <iostream>
#include <iomanip>
// A parameterized <iomanip> manipulator is not a function pointer at all --
// it is a library struct consumed by a template `operator<<` -- so it lands
// on the generic operand-type rejection naming the struct.
// MANIPSETW: manipulator-setw.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a std::ostream << operand of type '_Setw' is not a recognized output type
int use(void) {
  std::cout << std::setw(4) << 1;
  return 0;
}

//--- long-double.cpp
#include <iostream>
// The importer maps `long double` to f64, which cannot represent the range
// `operator<<(long double)` prints (`1e400L` prints `1e+400`; f64 saturates
// to `inf`). Inheriting the lossy `%Lg` mapping here would be a measured
// divergence, so the overload stays out.
// LONGDOUBLE: long-double.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a std::ostream << operand of type 'long double' is not a recognized output type
int use(long double v) {
  std::cout << v;
  return 0;
}

//--- string-view.cpp
#include <iostream>
#include <string_view>
// A `std::string_view` local is DECOMPOSED by the importer (backing array +
// cursor + length): there is no single Rust value to print. The generic
// operand-type rejection names the overload's parameter type.
// STRINGVIEW: string-view.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a std::ostream << operand of type 'basic_string_view<char, std::char_traits<char>>' is not a recognized output type
int use(void) {
  std::string_view sv = "abc";
  std::cout << sv;
  return 0;
}

//--- non-ascii-literal.cpp
#include <iostream>
// A literal operand folds into the Rust format string, so it inherits the
// same guards a printf format carries: a non-ASCII byte would reach the
// generated Rust source verbatim and fail rustc's UTF-8 check.
// NONASCII: non-ascii-literal.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: non-printable or non-ASCII byte in a std::ostream << string literal
int use(void) {
  std::cout << "caf\xc3\xa9";
  return 0;
}

//--- other-stream.cpp
#include <iostream>
// ALREADY FENCED, and deliberately so: the recognizer keys on the base
// VarDecl's NAME being exactly `cout`/`cerr`, never on "is it an ostream?".
// A type test would wrongly admit `std::clog` (unit-buffered, and its
// interleaving with cout is not modeled) and every user-declared ostream.
// OTHERSTREAM: other-stream.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference to 'clog' declared in a system header; not part of the supported C subset
int use(void) {
  std::clog << 1;
  return 0;
}

//--- ostream-param.cpp
#include <iostream>
// ALREADY FENCED by `mapStdLibraryType`'s tail, at the PARAMETER TYPE,
// before any call is looked at. W2.22 deliberately does NOT add
// `basic_ostream` to the recognized-STL table: leaving it unrecognized is
// exactly what keeps a function taking an ostream out, since passing the
// stream around has no image at all.
// OSTREAMPARAM: ostream-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_ostream is not a recognized STL type
void f(std::ostream &o) { (void)o; }

//--- user-operator.cpp
#include <iostream>
// ALREADY FENCED by the same tail, at the overload's own SIGNATURE, so the
// whole TU rejects loudly rather than a call site silently picking a
// built-in overload. A user `operator<<` is the ostream-dispatch feature in
// miniature; it needs the ostream value W2.22 does not model.
// USEROP: user-operator.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_ostream is not a recognized STL type
struct T {
  int v;
};
std::ostream &operator<<(std::ostream &o, const T &t) { return o << t.v; }

//--- ostream-ref-bind.cpp
#include <iostream>
// ALREADY FENCED by `mapType`'s reference case. Naming a stream keeps the
// chain base out of the recognized `cout`/`cerr` set too.
// REFBIND: ostream-ref-bind.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference types are not yet supported
int use(void) {
  std::ostream &o = std::cout;
  (void)o;
  return 0;
}

//--- conditional-stream.cpp
#include <iostream>
// ALREADY FENCED. A runtime-selected stream is not a `DeclRefExpr` base, so
// the chain never matches; the residual diagnostic is the generic place
// rejection on the conditional.
// CONDSTREAM: conditional-stream.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported assignable expression: ConditionalOperator
int use(int a) {
  (a > 1 ? std::cout : std::cerr) << 1;
  return 0;
}

//--- chain-as-condition.cpp
#include <iostream>
// ALREADY FENCED. Testing a stream reads its state through
// `operator bool`, which is a user-defined conversion.
// CHAINCOND: chain-as-condition.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported cast (UserDefinedConversion)
int use(void) {
  while (std::cout << 1) {
    break;
  }
  return 0;
}

//--- ostringstream.cpp
#include <sstream>
// ALREADY FENCED. Only the two named globals are streams here; a string
// stream is an unrecognized STL type, rejected at its declaration.
// OSTRINGSTREAM: ostringstream.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::basic_ostringstream is not a recognized STL type
int use(void) {
  std::ostringstream o;
  o << 1;
  return 0;
}

//--- using-namespace.cpp
#include <iostream>
// ALREADY FENCED, and the practical reach limit of this wave: the ONLY
// spelling that can reach the recognizer is the fully-qualified
// `std::cout`/`std::cerr`, because both `using namespace std;` and
// `using std::cout;` are rejected before any statement is walked.
// USINGNS: using-namespace.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported top-level declaration
using namespace std;
int use(void) {
  cout << 1;
  return 0;
}
