// FR-252: the closure ALIAS shapes rustc rejects, or that C++ and Rust would
// order differently, are LOCATED refusals, never left to rustc. A
// by-reference closure borrows its captures from its creation to its LAST
// call (NLL): a capture it MODIFIES may not be mentioned in between (E0499 /
// E0502 / E0503), one it only reads may not be modified or moved (E0502 /
// E0505 / E0506) -- including a `push_back` on a vector a live closure reads
// (iterator invalidation in C++ besides), an assignment target (not a Var
// node: FR-261's verification lesson), a call that takes it by `T&`, and
// another closure that modifies it. A by-copy capture of a vector / string is
// C++'s copy and Rust's move: refused when the variable is mentioned later or
// the closure is made in a loop the variable outlives. A closure that
// modifies state called twice in one statement without a sequence point
// (`cnt() * 10 + cnt()`; `&&` / `||` / `?:` DO sequence). The two loop shapes
// a closure would otherwise turn into SILENT miscompiles (C++ re-evaluates `i
// < n` every iteration, Rust's range once): a counting loop whose body calls a
// closure that modifies the bound `n` or pushes onto the vector behind `i <
// v.size()`; and a range-for whose body calls a closure that pushes onto the
// iterated vector. Also a closure that declares a name of its enclosing scope
// (a parameter named like an outer variable), one that moves its capture out
// (Rust FnOnce), and one that modifies state passed to a `const F&` parameter.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}17{{$}}
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

template <typename F>
int apply(const F& f, int x) {
    return f(x);
}

void touch(int& x) {
    x += 1;
}

int borrowed_vector(std::vector<int>& out) {
    std::vector<int> v = {1, 2};
    auto first = [&v]() -> int {
        return v[0];
    };
    // CHECK:      closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'v' is modified or moved while closure 'first', which captures it by reference, is still called later (a borrow conflict in Rust)
    v.push_back(3);
    int a = first();
    std::vector<int> w = {4};
    auto len = [&w]() -> std::size_t {
        return w.size();
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'w' is modified or moved while closure 'len', which captures it by reference, is still called later (a borrow conflict in Rust)
    out = std::move(w);
    std::size_t n = len();
    return a + static_cast<int>(n);
}

int borrowed_scalar(int seed) {
    int acc = seed;
    auto bump = [&acc](int by) {
        acc = acc + by;
    };
    bump(1);
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:16: error: unsupported: 'acc' is used while closure 'bump', which modifies it through a by-reference capture, is still called later (a second borrow in Rust)
    int seen = acc;
    bump(2);
    int k = seed;
    auto read = [&k]() -> int {
        return k;
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'k' is modified or moved while closure 'read', which captures it by reference, is still called later (a borrow conflict in Rust)
    k = 5;
    touch(k);
    int r = read();
    int m = seed;
    auto get = [&m]() -> int {
        return m;
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'm' is modified or moved while closure 'get', which captures it by reference, is still called later (a borrow conflict in Rust)
    auto set = [&m](int x) {
        m = x;
    };
    set(get());
    return seen + r;
}

int copies(int seed) {
    std::vector<int> v = {seed};
    std::string s = "x";
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: 'v' is used after closure 'peek' captures it by copy (C++ copies it, Rust moves it into the closure: the subset requires no later mention of 'v')
    auto peek = [v, s]() -> int {
        return v[0] + static_cast<int>(s.size());
    };
    int a = peek() + static_cast<int>(v.size());
    std::vector<int> keep = {1};
    for (int i = 0; i < 3; ++i) {
        // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:9: error: unsupported: closure 'f' captures 'keep' by copy inside a loop 'keep' was declared outside of (Rust moves it on the first iteration)
        auto f = [keep](int j) -> int {
            return keep[0] + j;
        };
        a += f(i);
    }
    return a;
}

int unsequenced(int seed) {
    int k = seed;
    auto cnt = [k]() mutable -> int {
        k = k + 1;
        return k;
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:26: error: unsupported: closure 'cnt', which modifies state, is called twice in one statement without a sequence point between the calls (C++ leaves the order unspecified)
    int r = cnt() * 10 + cnt();
    bool ok = cnt() > 3 && cnt() > 4;
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:35: error: unsupported: closure 'cnt', which modifies state, is called twice in one statement without a sequence point between the calls (C++ leaves the order unspecified)
    std::printf("%d %d\n", cnt(), cnt());
    return r + (ok ? 1 : 0);
}

int loops(int n) {
    auto shrink = [&n]() {
        n = n - 1;
    };
    int total = 0;
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+2]]:5: error: unsupported: counting loop body assigns or shadows the bound 'n'
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:25: error: unsupported: 'n' is used while closure 'shrink', which modifies it through a by-reference capture, is still called later (a second borrow in Rust)
    for (int i = 0; i < n; ++i) {
        shrink();
        total += i;
    }
    std::vector<int> v = {1, 2, 3};
    auto grow = [&v](int x) {
        v.push_back(x);
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+2]]:5: error: unsupported: counting loop body modifies or shadows 'v', whose size is the bound
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:33: error: unsupported: 'v' is used while closure 'grow', which modifies it through a by-reference capture, is still called later (a second borrow in Rust)
    for (std::size_t i = 0; i < v.size(); ++i) {
        grow(1);
    }
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: the body of a range-for over 'v' modifies 'v' (iterator invalidation in C++, a borrow conflict in Rust)
    for (int e : v) {
        grow(e);
    }
    return total;
}

int misc(int seed) {
    int q = seed;
    auto shadow = [&q](int seed2) -> int {
        int fixed2 = q + seed2;
        return fixed2;
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:5: error: unsupported: closure 'shadow2' declares 'seed', which names a variable of the enclosing scope (a capture and a local must not share a name)
    auto shadow2 = [q](int seed) -> int {
        return q + seed;
    };
    std::vector<int> v = {1};
    auto steal = [&v]() -> std::size_t {
        // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:30: error: unsupported: closure 'steal' moves its capture 'v' out (Rust makes it FnOnce: callable once)
        std::vector<int> w = std::move(v);
        return w.size();
    };
    int acc = 0;
    auto add = [&acc](int x) -> int {
        acc = acc + x;
        return acc;
    };
    // CHECK-NEXT: closure-alias.cpp:[[@LINE+1]]:19: error: unsupported: closure 'add' modifies state but is passed to a `const F&` parameter of 'apply' (Rust's `&impl Fn` cannot call it; normalizer: take it as `F&`)
    int r = apply(add, 3);
    return shadow(1) + shadow2(2) + static_cast<int>(steal()) + r;
}

int main() {
    return 0;
}
