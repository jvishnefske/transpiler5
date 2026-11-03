// REQUIRES: cargo
// FR-254 R4: emitrust-bx --normalize --project-root, C++ -> Rust, byte-diffed
// against clang++ of the ORIGINAL TWO-FILE program (this file plus its
// project header Inputs/bx-normalize-header.h) -- importing header helpers
// into one unity translation unit must not change behaviour. Rows: `inline`
// big-endian store helpers called with a running offset (woff2's
// Store16/StoreU32 shape), a `static` header helper reached only THROUGH
// another header function (LoadU32 -> Load8, transitive import), a header
// helper whose own `for (; n != 0; n >>= 1)` loop is rewritten by R1 inside
// the imported body, a helper inside a namespace called from a main-file
// function of the same namespace, and an uncalled header function that the
// lazy import never brings in. The subset has no `main(argc, ...)`, so the
// values come from an LCG; UB-free (every store stays inside its buffer,
// shifts are on unsigned values below 32 bits).
// RUN: %emitrust-bx --normalize --project-root=%S --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_normalize_header > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: not grep -q unsafe %t.crate/src/main.rs
// RUN: not grep -q NeverCalled %t.crate/src/main.rs
#include <cstdio>
#include <vector>

#include "Inputs/bx-normalize-header.h"

namespace codec {
int Fold(int acc, int v) {
    return Mix(acc, v % 1000);
}
}  // namespace codec

int next(int x) {
    return (x * 1103 + 12345) % 30011;
}

int main() {
    std::vector<std::uint8_t> buf(96, 0);
    int seed = 7;
    std::size_t off = 0;
    for (int i = 0; i < 12; ++i) {
        seed = next(seed);
        off = Store16(buf.data(), off, seed);
        seed = next(seed);
        std::uint32_t w = static_cast<std::uint32_t>(seed) * 131071u + 17u;
        off = StoreU32(buf.data(), off, w);
    }
    std::printf("stored %zu bytes\n", off);
    int acc = 1;
    std::size_t pos = 2;
    for (int i = 0; i < 12; ++i) {
        std::uint32_t w = LoadU32(buf.data(), pos);
        std::printf("%u %d\n", w, Log2Floor(w));
        acc = codec::Fold(acc, static_cast<int>(w % 30011u));
        pos = pos + 6;
    }
    std::printf("acc %d\n", acc);
    std::printf("log2 %d %d %d\n", Log2Floor(1u), Log2Floor(1024u), Log2Floor(4294967295u));
    return 0;
}
