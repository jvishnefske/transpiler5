// FR-261: Rust slices -> C++ pointers, law 2 at the CLI from the Rust side
// (the C++ printed for this file, fed back in, prints byte-identical C++; its
// Rust is this file's canonical Rust). Pins that rs_parse reads `&[T]` /
// `&mut [T]` parameters and locals as `const T*` / `T*` (a non-`mut` binding
// is top-level const, `T* const`), `&data[..]` as `data.data()`, `&mut
// big[1usize..]` as `big.data() + 1`, `&p[n..]` on a pointer local as `p + n`,
// `out = &mut out[1usize..];` as `++out;` (normal form N18 then prints the
// increment), `std::slice::from_ref(&lim)` / `from_mut(&mut cur)` as `&lim` /
// `&cur`, `std::slice::from_mut(self)` as `this`, `p[i]` / `pivot[0usize]` /
// `c[0usize].sum` as `p[i]` / `*pivot` / `c->sum`, and a member call through a
// pointer `c[0usize].take(data)` as `c->take(data)`; `println!` is accepted
// and printed back as `print!`.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
// CHECK:      mod buf {
// CHECK-NEXT:     #[derive(Clone)]
// CHECK-NEXT:     pub struct Cursor {
// CHECK-NEXT:         pub pos: usize,
// CHECK-NEXT:         pub sum: u32,
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     impl Cursor {
// CHECK-NEXT:         pub fn take(&mut self, data: &[u8]) -> u8 {
// CHECK-NEXT:             let b: u8 = data[self.pos];
// CHECK-NEXT:             self.pos = self.pos.wrapping_add(1usize);
// CHECK-NEXT:             self.sum = self.sum.wrapping_add(b as u32);
// CHECK-NEXT:             return b;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn drain(&mut self, data: &[u8], n: usize) {
// CHECK-NEXT:             for i in 0usize..n {
// CHECK-NEXT:                 take_into(std::slice::from_mut(self), data, i);
// CHECK-NEXT:             }
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     pub fn take_into(c: &mut [Cursor], data: &[u8], k: usize) {
// CHECK-NEXT:         let b: u8 = c[0usize].take(data);
// CHECK-NEXT:         c[0usize].sum = c[0usize].sum.wrapping_add((b as u32).wrapping_mul(k as u32));
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn fill(mut out: &mut [i64], n: usize, mut v: i64) {
// CHECK-NEXT:     let mut i: usize = 0usize;
// CHECK-NEXT:     while i < n {
// CHECK-NEXT:         out[0usize] = v;
// CHECK-NEXT:         out = &mut out[1usize..];
// CHECK-NEXT:         v *= 3i64;
// CHECK-NEXT:         i = i.wrapping_add(1usize);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn count(xs: &[i64], n: usize, pivot: &[i64]) -> i32 {
// CHECK-NEXT:     let mut c: i32 = 0;
// CHECK-NEXT:     let mut p: &[i64] = xs;
// CHECK-NEXT:     for i in 0usize..n {
// CHECK-NEXT:         if p[i] > pivot[0usize] {
// CHECK-NEXT:             c += 1;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     p = &p[n..];
// CHECK-NEXT:     return c + (p[0usize] as i32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut data: Vec<u8> = vec![3u8, 1u8, 4u8, 1u8, 5u8, 9u8];
// CHECK-NEXT:     let mut cur: crate::buf::Cursor = crate::buf::Cursor { pos: 1usize, sum: 0u32 };
// CHECK-NEXT:     cur.drain(&data[..], 3usize);
// CHECK-NEXT:     crate::buf::take_into(std::slice::from_mut(&mut cur), &data[1usize..], 10usize);
// CHECK-NEXT:     data[0usize] = cur.take(&data[..]);
// CHECK-NEXT:     let mut big: Vec<i64> = vec![0i64; 6usize];
// CHECK-NEXT:     fill(&mut big[1usize..], 4usize, 2i64);
// CHECK-NEXT:     let lim: i64 = 10i64;
// CHECK-NEXT:     let n: i32 = count(&big[..], 5usize, std::slice::from_ref(&lim));
// CHECK-NEXT:     print!("{} {} {} {} {}\n", cur.pos, cur.sum, data[0usize] as i32, big[4usize], n);
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: namespace buf {
// CPP-EMPTY:
// CPP-NEXT: struct Cursor {
// CPP-NEXT:     std::size_t pos;
// CPP-NEXT:     unsigned sum;
// CPP-NEXT:     unsigned char take(const unsigned char* const data);
// CPP-NEXT:     void drain(const unsigned char* const data, const std::size_t n);
// CPP-NEXT: };
// CPP-EMPTY:
// CPP-NEXT: }  // namespace buf
// CPP-EMPTY:
// CPP-NEXT: namespace buf {
// CPP-NEXT: void take_into(Cursor* const c, const unsigned char* const data, const std::size_t k);
// CPP-NEXT: }  // namespace buf
// CPP-EMPTY:
// CPP-NEXT: namespace buf {
// CPP-EMPTY:
// CPP-NEXT: unsigned char Cursor::take(const unsigned char* const data) {
// CPP-NEXT:     const unsigned char b = data[this->pos];
// CPP-NEXT:     ++this->pos;
// CPP-NEXT:     this->sum += static_cast<unsigned>(b);
// CPP-NEXT:     return b;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Cursor::drain(const unsigned char* const data, const std::size_t n) {
// CPP-NEXT:     for (std::size_t i = static_cast<std::size_t>(0); i < n; ++i) {
// CPP-NEXT:         ::buf::take_into(this, data, i);
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void take_into(Cursor* const c, const unsigned char* const data, const std::size_t k) {
// CPP-NEXT:     const unsigned char b = c->take(data);
// CPP-NEXT:     c->sum += static_cast<unsigned>(b) * static_cast<unsigned>(k);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace buf
// CPP-EMPTY:
// CPP-NEXT: void fill(long long* out, const std::size_t n, long long v) {
// CPP-NEXT:     std::size_t i = static_cast<std::size_t>(0);
// CPP-NEXT:     while (i < n) {
// CPP-NEXT:         *out = v;
// CPP-NEXT:         ++out;
// CPP-NEXT:         v *= 3LL;
// CPP-NEXT:         ++i;
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int count(const long long* const xs, const std::size_t n, const long long* const pivot) {
// CPP-NEXT:     int c = 0;
// CPP-NEXT:     const long long* p = xs;
// CPP-NEXT:     for (std::size_t i = static_cast<std::size_t>(0); i < n; ++i) {
// CPP-NEXT:         if (p[i] > *pivot) {
// CPP-NEXT:             ++c;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     p += n;
// CPP-NEXT:     return c + static_cast<int>(*p);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<unsigned char> data = {static_cast<unsigned char>(3), static_cast<unsigned char>(1), static_cast<unsigned char>(4), static_cast<unsigned char>(1), static_cast<unsigned char>(5), static_cast<unsigned char>(9)};
// CPP-NEXT:     ::buf::Cursor cur = {static_cast<std::size_t>(1), 0U};
// CPP-NEXT:     cur.drain(data.data(), static_cast<std::size_t>(3));
// CPP-NEXT:     ::buf::take_into(&cur, data.data() + 1, static_cast<std::size_t>(10));
// CPP-NEXT:     data[static_cast<std::size_t>(0)] = cur.take(data.data());
// CPP-NEXT:     std::vector<long long> big(static_cast<std::size_t>(6), 0LL);
// CPP-NEXT:     fill(big.data() + 1, static_cast<std::size_t>(4), 2LL);
// CPP-NEXT:     const long long lim = 10LL;
// CPP-NEXT:     const int n = count(big.data(), static_cast<std::size_t>(5), &lim);
// CPP-NEXT:     std::printf("%zu %u %d %lld %d\n", cur.pos, cur.sum, static_cast<int>(data[static_cast<std::size_t>(0)]), big[static_cast<std::size_t>(4)], n);
// CPP-NEXT:     return 0;
// CPP-NEXT: }
mod buf {
    #[derive(Clone)]
    pub struct Cursor {
        pub pos: usize,
        pub sum: u32,
    }

    impl Cursor {
        pub fn take(&mut self, data: &[u8]) -> u8 {
            let b: u8 = data[self.pos];
            self.pos = self.pos.wrapping_add(1usize);
            self.sum = self.sum.wrapping_add(b as u32);
            return b;
        }

        pub fn drain(&mut self, data: &[u8], n: usize) {
            for i in 0usize..n {
                take_into(std::slice::from_mut(self), data, i);
            }
        }
    }

    pub fn take_into(c: &mut [Cursor], data: &[u8], k: usize) {
        let b: u8 = c[0usize].take(data);
        c[0usize].sum = c[0usize].sum.wrapping_add((b as u32).wrapping_mul(k as u32));
    }
}

fn fill(mut out: &mut [i64], n: usize, mut v: i64) {
    let mut i: usize = 0usize;
    while i < n {
        out[0usize] = v;
        out = &mut out[1usize..];
        v = v * 3i64;
        i = i.wrapping_add(1usize);
    }
}

fn count(xs: &[i64], n: usize, pivot: &[i64]) -> i32 {
    let mut c: i32 = 0;
    let mut p: &[i64] = xs;
    for i in 0usize..n {
        if p[i] > pivot[0usize] {
            c += 1;
        }
    }
    p = &p[n..];
    return c + (p[0usize] as i32);
}

fn main() {
    let mut data: Vec<u8> = vec![3u8, 1u8, 4u8, 1u8, 5u8, 9u8];
    let mut cur: crate::buf::Cursor = crate::buf::Cursor { pos: 1usize, sum: 0u32 };
    cur.drain(&data[..], 3usize);
    crate::buf::take_into(std::slice::from_mut(&mut cur), &data[1usize..], 10usize);
    data[0usize] = cur.take(&data[..]);
    let mut big: Vec<i64> = vec![0i64; 6usize];
    fill(&mut big[1usize..], 4usize, 2i64);
    let lim: i64 = 10i64;
    let n: i32 = count(&big[..], 5usize, std::slice::from_ref(&lim));
    println!("{} {} {} {} {}", cur.pos, cur.sum, data[0usize] as i32, big[4usize], n);
}
