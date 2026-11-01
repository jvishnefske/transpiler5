// FR-255: everything bc.hpp must REJECT AT COMPILE TIME, one split unit per
// case so each pins its own diagnostic (clang++ -std=c++20 -fsyntax-only; the
// same 12 cases were checked by hand to fail under g++ 15 too). The
// discipline is aliasing XOR mutation decided on TYPES: a mutable and a shared
// borrow of one Owner (alias), of two elements of a std::vector<Owner<int>>
// (vec-alias: same type, so conservatively one object, like Rust indexing); a
// capturing callback (capture) or a captureless one reaching another local
// Owner (local: the language's own implicit-capture rule); a result that
// carries the borrow out as a pointer (escape) or a reference (escape-ref);
// copying an Owner (copy); an Owner of a pointer (ptr) or of a user struct
// holding one (user-struct); a mutable borrow through a shared one
// (mut-through-ref); opening an Owner without bc::with (private: there is no
// accessor, only bc::detail::opener); and re-spelling a nested owner type
// (respelled: each written-out bc::Owner<X> is a NEW type -- the documented
// usability cost, name it once with `using`).
// RUN: split-file %s %t
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/alias.cpp 2>&1 | FileCheck --check-prefix=ALIAS %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/vec-alias.cpp 2>&1 | FileCheck --check-prefix=VECALIAS %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/capture.cpp 2>&1 | FileCheck --check-prefix=CAPTURE %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/local.cpp 2>&1 | FileCheck --check-prefix=LOCAL %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/escape.cpp 2>&1 | FileCheck --check-prefix=ESCAPE %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/escape-ref.cpp 2>&1 | FileCheck --check-prefix=ESCREF %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/copy.cpp 2>&1 | FileCheck --check-prefix=COPY %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/ptr.cpp 2>&1 | FileCheck --check-prefix=PTR %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/user-struct.cpp 2>&1 | FileCheck --check-prefix=USERSTRUCT %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/mut-through-ref.cpp 2>&1 | FileCheck --check-prefix=MUTREF %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/private.cpp 2>&1 | FileCheck --check-prefix=PRIVATE %s
// RUN: not clang++ -std=c++20 -fsyntax-only -I%bx_src/include %t/respelled.cpp 2>&1 | FileCheck --check-prefix=RESPELLED %s
// ALIAS: aliasing XOR mutation
// VECALIAS: aliasing XOR mutation
// CAPTURE: the callback must be a captureless lambda
// LOCAL: variable 'b' cannot be implicitly captured
// ESCAPE: !std::is_pointer_v<int *>
// ESCAPE-SAME: the callback's result may carry a borrow out of its scope
// ESCREF: !std::is_reference_v<int &>
// ESCREF-SAME: the callback's result may carry a borrow out of its scope
// COPY: call to deleted constructor of 'bc::Owner<int>'
// PTR: T may contain a reference or pointer
// USERSTRUCT: T may contain a reference or pointer
// MUTREF: no matching function for call to 'open'
// PRIVATE: 'value_' is a private member of 'bc::Owner<int>'
// RESPELLED: no viable conversion from 'bc::Owner<Point>'

//--- alias.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); bc::with(a, bc::ref(a), [](int& x, const int& y) { x = y; }); }
//--- vec-alias.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
using I = bc::Owner<int>;
int main() { std::vector<I> v; v.emplace_back(1); v.emplace_back(2);
  bc::with(v[0], bc::ref(v[1]), [](int& a, const int& b) { a += b; }); }
//--- capture.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); int k = 2; bc::with(a, [&](int& x) { x += k; }); }
//--- local.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1), b(2); bc::with(a, [](int& x) { bc::with(b, [](int& y) { y++; }); x++; }); }
//--- escape.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); int* p = bc::with(a, [](int& x) { return &x; }); (void)p; }
//--- escape-ref.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); bc::with(a, [](int& x) -> int& { return x; }); }
//--- copy.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); auto b = a; (void)b; }
//--- ptr.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int*> a(nullptr); }
//--- user-struct.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
struct Q { int* p; };
int main() { bc::Owner<Q> q(Q{nullptr}); }
//--- mut-through-ref.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
using P = bc::Owner<int>;
int main() { bc::Owner<std::pair<P, int>> n(P(1), 2);
  bc::with(bc::ref(n), [](const auto& pr) { bc::with(pr.first, [](int& x) { x = 0; }); }); }
//--- private.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
int main() { bc::Owner<int> a(1); return a.value_; }
//--- respelled.cpp
#include "bc.hpp"
#include <utility>
#include <vector>
struct Point { int x, y; };
BC_BORROW_FREE(Point, int, int);
int main() { bc::Owner<std::pair<bc::Owner<Point>, int>> n(bc::Owner<Point>(Point{1, 2}), 3); }
