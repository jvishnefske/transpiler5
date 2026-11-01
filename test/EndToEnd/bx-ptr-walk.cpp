// REQUIRES: cargo
// FR-261: emitrust-bx C++ -> Rust, pointer WALKING and pointer locals end to
// end (byte-diff vs clang++). `++p`, `p += k` and `p = p + k` on a `const T*`
// <=> `p = &p[k..];`, and on a `T*` parameter or local <=> `p = &mut p[k..];`
// (both measured to borrow-check on rustc 1.96 and 1.98); pointer locals
// `const T* q = p + k;` / `T* w = v.data() + k;` <=> `let q: &[T] = &p[k..];`
// / `let mut w: &mut [T] = &mut v[k..];`, a mutable local that goes out of
// scope before its vector is used again; a Huffman-style run writer through a
// `std::size_t*` tree size and two `std::uint8_t*` buffers with a byte
// reversal between indices (written from scratch after brunsli's shape); a
// signed counting-loop index (`p[i]` with `int i` from 0 <=> `p[i as usize]`,
// normal form N19). UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_ptr_walk > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

void Reverse(std::uint8_t* v, std::size_t start, std::size_t end) {
    end = end - 1;
    while (start < end) {
        const std::uint8_t tmp = v[start];
        v[start] = v[end];
        v[end] = tmp;
        ++start;
        --end;
    }
}

void WriteZeros(std::size_t reps, std::size_t* tree_size, std::uint8_t* tree, std::uint8_t* extra) {
    if (reps == 11) {
        tree[*tree_size] = 0;
        extra[*tree_size] = 0;
        ++*tree_size;
        --reps;
    }
    if (reps < 3) {
        for (std::size_t i = 0; i < reps; ++i) {
            tree[*tree_size] = 0;
            extra[*tree_size] = 0;
            ++*tree_size;
        }
    } else {
        reps -= 3;
        const std::size_t start = *tree_size;
        while (true) {
            tree[*tree_size] = 17;
            extra[*tree_size] = static_cast<std::uint8_t>(reps & 7);
            ++*tree_size;
            reps = reps >> 3;
            if (reps == 0) {
                break;
            }
            --reps;
        }
        Reverse(tree, start, *tree_size);
        Reverse(extra, start, *tree_size);
    }
}

unsigned Walk(const std::uint8_t* p, std::size_t n) {
    unsigned h = 7;
    while (n > 0) {
        h = h * 31 + *p;
        ++p;
        n -= 1;
    }
    return h;
}

void Fill(long long* out, std::size_t n, long long v) {
    while (n > 0) {
        *out = v;
        out += 1;
        v = v * 3 + 1;
        --n;
    }
}

long long Stride(const long long* p, std::size_t count, std::size_t step) {
    long long t = 0;
    const long long* q = p + 1;
    for (std::size_t i = 0; i < count; ++i) {
        t = t * 2 + *q;
        q = q + step;
    }
    return t + p[0];
}

int Weighted(const int* w, int n) {
    int t = 0;
    for (int i = 0; i < n; ++i) {
        t += w[i] * (i + 1);
    }
    return t;
}

int main() {
    std::size_t reps = 2;
    while (reps < 60) {
        std::vector<std::uint8_t> tree(32, 9);
        std::vector<std::uint8_t> extra(32, 8);
        std::size_t ts = 2;
        std::size_t first = reps;
        if (reps == 2) {
            first = 11;
        }
        WriteZeros(first, &ts, tree.data(), extra.data());
        std::printf("%zu:%zu:", reps, ts);
        for (std::size_t i = 0; i < ts; ++i) {
            std::printf("%d/%d ", static_cast<int>(tree[i]), static_cast<int>(extra[i]));
        }
        std::printf("\n");
        reps += 9;
    }
    std::vector<std::uint8_t> bytes = {5, 4, 3, 2, 1, 250, 251};
    std::printf("walk=%u half=%u none=%u\n", Walk(bytes.data(), bytes.size()), Walk(bytes.data() + 3, 4),
                Walk(bytes.data() + bytes.size(), 0));
    std::vector<long long> big(12, 0);
    {
        long long* w = big.data() + 2;
        Fill(w, 5, 4);
        w += 6;
        Fill(w, 2, 9);
    }
    std::printf("big=%lld %lld %lld %lld\n", big[2], big[6], big[8], big[9]);
    std::printf("stride=%lld %lld\n", Stride(big.data(), 3, 2), Stride(big.data() + 4, 2, 3));
    std::vector<int> ws = {3, 1, 4, 1, 5};
    std::printf("weighted=%d %d\n", Weighted(ws.data(), 5), Weighted(ws.data() + 2, 3));
    return 0;
}
