// REQUIRES: cargo
// FR-254 R3: emitrust-bx --normalize, C++ -> Rust, byte-diffed against
// clang++ of the ORIGINAL (un-normalized) source -- the rewrite must not
// change behaviour. Every `++` / `--` used as a value here is outside the
// subset without --normalize (it acts on `*p` or on a pointer local, not on
// a local integer), and R3 rewrites each statement to "read the old value in
// place, then a separate `++x;`" (`return E;` through a fresh `const T`).
// Rows: an index post-increment through a pointer PARAMETER (in a range-for,
// with a call on the right-hand side, as a printf argument, in a compound
// assignment) and through a pointer LOCAL; a post-DECREMENT in a
// declaration; a pointer post-increment store `*(out++) = v` and a const
// pointer walk `*(r++)`; a `return` of a post-increment read whose caller
// loops; R3 inside R1/R2-rewritten loops, including a `for` STEP that holds
// the site. The load-bearing order row: `dst[(*off)++] = Mix(..)` -- C++17
// sequences the right side before the whole left side, the rewrite reads
// `*off` at the same point. UB-free (every index stays inside its buffer,
// accumulators reduced mod 10007), inputs from an LCG.
// RUN: %emitrust-bx --normalize --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_normalize_incdec > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: not grep -q unsafe %t.crate/src/main.rs
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

int next(int x) {
    return (x * 1103 + 12345) % 30011;
}

int Mix(int a, int b) {
    return (a * 31 + b) % 113;
}

void Pack(std::uint8_t* dst, std::size_t* off, const std::vector<int>& v) {
    for (int x : v) {
        const std::uint8_t b = static_cast<std::uint8_t>(x & 0xff);
        dst[(*off)++] = b;
    }
}

void Marks(int* dst, std::size_t* off, int n) {
    for (int i = 0; i < n; ++i) {
        dst[(*off)++] = Mix(i, n);
    }
}

void Base128(std::uint8_t* out, unsigned val, int len) {
    for (int i = 0; i < len; ++i) {
        *(out++) = static_cast<std::uint8_t>((val & 0x7fU) | (i + 1 < len ? 0x80U : 0U));
        val >>= 7;
    }
}

int ReadByte(const std::uint8_t* data, std::size_t* pos) {
    return data[(*pos)++];
}

int SumBytes(const std::uint8_t* data, std::size_t n) {
    std::size_t pos = 0;
    int acc = 0;
    while (pos < n) {
        acc = (acc * 7 + ReadByte(data, &pos)) % 10007;
    }
    return acc;
}

int Walk(const int* d, int n) {
    const int* r = d;
    int acc = 1;
    for (int i = 0; i < n; ++i) {
        acc = (acc * 3 + *(r++)) % 10007;
    }
    return acc;
}

int Backwards(const int* d, std::size_t* pos, int n) {
    int acc = 0;
    for (int i = 0; i < n; i += 2) {
        const int v = d[(*pos)--];
        acc = (acc * 5 + v + i) % 10007;
    }
    return acc;
}

void Strided(int* d, std::size_t* p, int n) {
    for (int i = n; i > 0; d[(*p)++] = i) {
        i -= 3;
    }
    int k = n;
    do {
        d[(*p)++] += k;
        k -= 4;
    } while (k > 0);
}

void ThroughLocal(int* d, std::size_t* p, int n) {
    std::size_t* q = p;
    for (int i = 0; i < n; ++i) {
        d[(*q)++] = i * i % 17;
    }
}

void Show(const int* d, std::size_t* p, int n) {
    for (int i = 0; i < n; ++i) {
        std::printf("%d ", d[(*p)++]);
    }
    std::printf("\n");
}

int Checksum(const std::vector<int>& v) {
    int acc = 0;
    for (int x : v) {
        acc = (acc * 31 + x) % 10007;
    }
    return acc;
}

int main() {
    int x = 29;
    for (int r = 0; r < 6; ++r) {
        x = next(x);
        const int n = x % 23;
        std::vector<int> vals;
        for (int i = 0; i < n; ++i) {
            vals.push_back((x + i * 37) % 1000);
        }
        std::vector<std::uint8_t> bytes(64, 0);
        std::size_t off = static_cast<std::size_t>(r);
        Pack(bytes.data(), &off, vals);
        Base128(bytes.data() + 40, static_cast<unsigned>(x) * 977U, 4);
        std::printf("pack %d %d %d\n", static_cast<int>(off), SumBytes(bytes.data(), 64), bytes[41]);
        std::vector<int> ints(96, 2);
        std::size_t p = static_cast<std::size_t>(r % 3);
        Marks(ints.data(), &p, n);
        Strided(ints.data(), &p, n + 5);
        ThroughLocal(ints.data(), &p, n);
        std::printf("ints %d %d walk %d\n", static_cast<int>(p), Checksum(ints), Walk(ints.data() + 2, n));
        std::size_t back = static_cast<std::size_t>(n + 30);
        const int bw = Backwards(ints.data(), &back, n);
        std::printf("back %d %d\n", bw, static_cast<int>(back));
        std::size_t s = static_cast<std::size_t>(r);
        Show(ints.data(), &s, 4);
    }
    return 0;
}
