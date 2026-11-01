// REQUIRES: cargo
// FR-261: emitrust-bx C++ -> Rust, single-object RECORD pointers end to end
// (byte-diff vs clang++) -- the lever the spike measured (167 of the 276
// probe-observed woff2/brunsli functions take a `State*`). A `State* s` is a
// one-element slice: `s->f` <=> `s[0usize].f` read, written and
// compound-assigned, `s->m(..)` <=> `s[0usize].m(..)` for const and non-const
// member functions, `s->bytes.push_back(b)` / `s->bytes.size()` through it,
// `&w` <=> `std::slice::from_mut(&mut w)` at the call site, a pointer passed
// on to a helper (`PutBits(w, ..)` inside `PutByte`), `this` handed to a free
// function from a member (`Flush(this)` <=> `from_mut(self)`), the address of
// a data member (`&w.nbits` <=> `from_mut(&mut w.nbits)`) and a `const
// State*` reader. UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ptr_state > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

struct BitWriter;
void Flush(BitWriter* w);

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    unsigned acc;
    int nbits;
    std::size_t calls;
    int pending() const { return nbits; }
    void finish() {
        Flush(this);
        calls += 100;
    }
};

void PutBits(BitWriter* w, int n, unsigned v) {
    w->acc = w->acc | ((v & ((1U << n) - 1U)) << w->nbits);
    w->nbits += n;
    w->calls = w->calls + 1;
    while (w->nbits >= 8) {
        w->bytes.push_back(static_cast<std::uint8_t>(w->acc & 0xffU));
        w->acc = w->acc >> 8;
        w->nbits -= 8;
    }
}

void PutByte(BitWriter* w, unsigned b) {
    PutBits(w, 8, b);
}

void Flush(BitWriter* w) {
    if (w->pending() > 0) {
        const int pad = 8 - w->pending();
        PutBits(w, pad, 0);
    }
}

unsigned Checksum(const BitWriter* w) {
    unsigned h = 17;
    for (std::size_t i = 0; i < w->bytes.size(); ++i) {
        h = h * 31 + w->bytes[i];
    }
    return h + static_cast<unsigned>(w->calls);
}

void Bump(int* n, int by) {
    *n = *n + by;
}

int main() {
    BitWriter w = {{}, 0, 0, 0};
    PutBits(&w, 3, 5);
    PutBits(&w, 7, 99);
    PutByte(&w, 200);
    PutBits(&w, 4, 9);
    std::printf("pending=%d bytes=%zu\n", w.pending(), w.bytes.size());
    w.finish();
    Bump(&w.nbits, 11);
    std::printf("bytes=%zu nbits=%d calls=%zu sum=%u\n", w.bytes.size(), w.nbits, w.calls, Checksum(&w));
    for (std::size_t i = 0; i < w.bytes.size(); ++i) {
        std::printf("%d ", static_cast<int>(w.bytes[i]));
    }
    std::printf("\n");
    return 0;
}
