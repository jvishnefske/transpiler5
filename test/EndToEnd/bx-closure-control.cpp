// REQUIRES: cargo
// FR-252: emitrust-bx C++ -> Rust, closures with CONTROL FLOW end to end
// (byte-diff vs clang++): early `return` from inside loops of a closure body
// (it returns from the closure, not the enclosing function), `while` / `if`
// / counting loops in a body, a closure called in a `while` condition and
// under `&&` / `?:` (laziness: the skipped call must not run -- each call
// bumps a by-reference counter), an increment used as a value in a mutable
// closure, unsigned wrapping inside a body, and closures in a namespaced
// function. UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_closure_control > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <vector>

int first_over(const std::vector<int>& v, int limit) {
    auto find = [&v](int lim) -> int {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] > lim) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    int a = find(limit);
    int b = find(limit * 2);
    int c = find(1000);
    std::printf("first_over: %d %d %d\n", a, b, c);
    return a + b + c;
}

int lazy(int seed) {
    int calls = 0;
    auto probe = [&calls](int x) -> bool {
        calls += 1;
        return x % 3 == 0;
    };
    int hits = 0;
    int i = seed;
    while (probe(i) || i < seed + 4) {
        if (i > seed + 8) {
            break;
        }
        hits += 1;
        i += 1;
    }
    bool both = probe(9) && probe(10);
    bool skip = probe(11) && probe(12);
    int pick = probe(seed) ? 100 : 200;
    std::printf("lazy: hits=%d both=%d skip=%d pick=%d calls=%d\n", hits, both ? 1 : 0, skip ? 1 : 0, pick, calls);
    return calls;
}

unsigned hashes(unsigned seed) {
    unsigned h = seed;
    auto mix = [&h](unsigned x) {
        h = h * 31U + x;
        h ^= h >> 7;
    };
    for (int k = 0; k < 5; ++k) {
        mix(static_cast<unsigned>(k) * 2654435761U);
    }
    std::printf("hashes: %u\n", h);
    return h;
}

int serial(int start) {
    int next_id = start;
    auto take = [next_id]() mutable -> int {
        return next_id++;
    };
    auto peek = [next_id]() mutable -> int {
        return ++next_id;
    };
    int a = take();
    int b = take();
    int c = peek();
    int d = peek();
    std::printf("serial: %d %d %d %d start=%d\n", a, b, c, d, next_id);
    return a + b + c + d;
}

namespace geo {
int clamp_all(std::vector<int>& xs, int lo, int hi) {
    int changed = 0;
    auto clamp = [lo, hi](int x) -> int {
        if (x < lo) {
            return lo;
        } else if (x > hi) {
            return hi;
        }
        return x;
    };
    auto note = [&changed](bool c) {
        if (c) {
            changed += 1;
        }
    };
    for (int& x : xs) {
        int y = clamp(x);
        note(y != x);
        x = y;
    }
    return changed;
}
}  // namespace geo

int main() {
    std::vector<int> v = {3, 8, 1, 15, 22};
    int f = first_over(v, 7);
    int l = lazy(4);
    unsigned h = hashes(17U);
    int s = serial(40);
    int c = geo::clamp_all(v, 2, 20);
    std::printf("main: %d %d %u %d %d [%d %d %d %d %d]\n", f, l, h, s, c, v[0], v[1], v[2], v[3], v[4]);
    return 0;
}
