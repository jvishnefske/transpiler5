// REQUIRES: cargo
// FR-261: emitrust-bx C++ -> Rust, byte-buffer helpers end to end (byte-diff
// vs clang++). Written from scratch in the shape of the woff2/brunsli helpers
// the spike measured: a base-128 varint writer `StoreBase128(len, off, dst)`
// through a `std::size_t*` cursor and a `uint8_t*` buffer, the matching
// reader, and big-endian `ReadUint16` / `ReadUint8` over `const uint8_t*` with
// a `std::size_t*` position. Every pointer is the suffix slice of its
// allocation (`v.data()` <=> `&mut v[..]`, `&pos` <=> `from_mut(&mut pos)`),
// and lengths stay ordinary integers: `SumThenPeek(p, n)` reads n elements
// and then p[n] -- defined C++ because the LOGICAL count n is less than the
// allocation, which folding n into the slice would have broken (the spike's
// p2_m1 panic); `SumN(v.data() + v.size(), 0)` passes the one-past-end
// pointer (an empty suffix slice) and reads nothing; an empty vector's data()
// likewise. `WriteUShort(std::vector<uint8_t>* out, ..)` appends through a
// pointer to a vector (`out->push_back` <=> `out[0usize].push`; the pointee
// spelling `std::vector<uint8_t> *` has no desugared form in clang's JSON, so
// its typedef is resolved by the importer). UB-free (no index reaches the end of its allocation); distinct
// values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ptr_buffers > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

std::size_t Base128Size(std::size_t n) {
    std::size_t size = 1;
    while (n >= 128) {
        ++size;
        n = n >> 7;
    }
    return size;
}

void StoreBase128(std::size_t len, std::size_t* off, std::uint8_t* dst) {
    const std::size_t size = Base128Size(len);
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t b = (len >> (7 * (size - i - 1))) & 0x7f;
        if (i < size - 1) {
            b = b | 0x80;
        }
        dst[*off] = static_cast<std::uint8_t>(b);
        *off += 1;
    }
}

bool ReadBase128(const std::uint8_t* buf, std::size_t end, std::size_t* pos, std::size_t* value) {
    std::size_t result = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        if (*pos >= end) {
            return false;
        }
        const std::size_t code = buf[*pos];
        *pos += 1;
        result = (result << 7) | (code & 0x7f);
        if ((code & 0x80) == 0) {
            *value = result;
            return true;
        }
    }
    return false;
}

int ReadUint8(const std::uint8_t* data, std::size_t* pos) {
    const int v = data[*pos];
    *pos += 1;
    return v;
}

int ReadUint16(const std::uint8_t* data, std::size_t* pos) {
    const int v = (static_cast<int>(data[*pos]) << 8) + static_cast<int>(data[*pos + 1]);
    *pos += 2;
    return v;
}

void WriteUShort(std::vector<std::uint8_t>* out, int value) {
    out->push_back(static_cast<std::uint8_t>(value >> 8));
    out->push_back(static_cast<std::uint8_t>(value & 255));
}

unsigned SumN(const std::uint8_t* p, std::size_t n) {
    unsigned s = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s = s + p[i];
    }
    return s;
}

unsigned SumThenPeek(const std::uint8_t* p, std::size_t n) {
    return SumN(p, n) * 1000 + p[n];
}

int main() {
    std::vector<std::uint8_t> buf(64, 0);
    std::size_t off = 3;
    std::vector<std::size_t> vals = {0, 1, 127, 128, 300, 16383, 16384, 2097151, 268435455, 4000000000};
    for (std::size_t v : vals) {
        StoreBase128(v, &off, buf.data());
    }
    std::printf("off=%zu\n", off);
    std::size_t pos = 3;
    std::size_t value = 7;
    while (ReadBase128(buf.data(), off, &pos, &value)) {
        std::printf("%zu,", value);
    }
    std::printf("\npos=%zu\n", pos);
    std::size_t rpos = 0;
    while (rpos + 2 <= off) {
        std::printf("%d ", ReadUint16(buf.data(), &rpos));
    }
    if (rpos < off) {
        std::printf("%d", ReadUint8(buf.data(), &rpos));
    }
    std::printf("\n");
    std::vector<std::uint8_t> eight = {11, 12, 13, 14, 15, 16, 17, 18};
    std::printf("peek=%u mid=%u tail=%u\n", SumThenPeek(eight.data(), 4), SumThenPeek(eight.data() + 2, 3),
                SumThenPeek(eight.data() + 6, 1));
    std::printf("end=%u all=%u\n", SumN(eight.data() + eight.size(), 0), SumN(eight.data(), eight.size()));
    std::vector<std::uint8_t> empty;
    std::printf("empty=%u\n", SumN(empty.data(), empty.size()));
    WriteUShort(&empty, 4660);
    WriteUShort(&empty, 65021);
    std::printf("ushort=%zu %u\n", empty.size(), SumN(empty.data(), empty.size()));
    return 0;
}
