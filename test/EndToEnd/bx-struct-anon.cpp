// REQUIRES: cargo
// FR-257: emitrust-bx C++ -> Rust, a struct in an ANONYMOUS namespace inside
// a named one end to end (byte-diff vs clang++) -- the shape real code uses
// for file-local helper classes (woff2's GlyfEncoder, brunsli's
// BitReaderState). It is a private Rust `struct` in `mod codec` whose fields
// and member functions stay `pub`; its member functions are defined out of
// line from the enclosing `namespace codec` (C++ allows a definition in any
// enclosing namespace); functions of the same namespace -- internal or not --
// name it bare. A root anonymous namespace only changes linkage (normal form
// N14), so `Tally` is an ordinary root struct. UB-free; distinct values at
// every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_struct_anon > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <vector>

namespace {

struct Tally {
    int hits;
    int misses;
};

}  // namespace

namespace codec {
namespace {

struct Reader {
    std::vector<int> data;
    std::size_t pos;
    int sum;

    bool done() const {
        return pos >= data.size();
    }
    int next() {
        const int v = data[pos];
        pos += 1;
        sum += v;
        return v;
    }
};

int drain(Reader& r, Tally& t) {
    int last = 0;
    while (!r.done()) {
        last = r.next();
        if (last % 2 == 0) {
            t.hits += 1;
        } else {
            t.misses += 1;
        }
    }
    return last;
}

}  // namespace

int decode(int seed, Tally& t) {
    Reader r = {{seed, seed * 3, seed + 7, 4}, 0, 0};
    const int first = r.next();
    const int last = drain(r, t);
    return first * 1000 + last * 10 + r.sum;
}

}  // namespace codec

int main() {
    Tally t = {0, 0};
    const int a = codec::decode(5, t);
    const int b = codec::decode(12, t);
    std::printf("%d %d hits=%d misses=%d\n", a, b, t.hits, t.misses);
    return 0;
}
