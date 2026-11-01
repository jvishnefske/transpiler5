// FR-261: pointers <=> suffix slices, C++ -> canonical Rust under
// --check-roundtrip (laws 1-2 on BOTH sides), plus the canonical C++. Every
// pointer is the SUFFIX SLICE of its allocation: `const T*` <=> `&[T]`,
// `T*` <=> `&mut [T]` (parameters and locals; top-level const after the star,
// `T* const p` <=> `p: &mut [T]`). Pins: `p[i]` <=> `p[i]` with the index
// usize in the Kernel (normal form N19: a literal prints bare `p[2]`, an
// unsigned or provably non-negative signed one -- a counting-loop variable
// with a literal lower bound -- is `static_cast<std::size_t>(i)` <=>
// `i as usize`); `*p`, `p[0]`, `(*p).y` and `p->x` are one term (N16; the
// canonical C++ is `*p` / `p->f` / `p->m(..)` / `p->size()`, the Rust
// `p[0usize]`, `p[0usize].f`); `++*p` through a `std::size_t*` out-param;
// `p + k` and `v.data() + k` <=> `&p[k..]` / `&mut v[k..]` and `v.data()` <=>
// `&mut v[..]` (the `&` / `&mut` follows the PARAMETER, not the base); `&x`
// <=> `std::slice::from_mut(&mut x)`, `&v[0]` is the suffix `v.data() + 0`
// (N17), `this` <=> `std::slice::from_mut(self)`; `++p`, `p++`, `p += k`
// and `p = p + 0` are one re-slice (N18) <=> `p = &p[k..];` (`&mut` for a
// `T*`); a `const T* const` local; a pointer to a std::vector / std::string
// (`v->push_back(n)`, `(*v)[0] = 3`, `s->c_str()`).
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      mod geo {
// CHECK-NEXT:     #[derive(Clone)]
// CHECK-NEXT:     pub struct Pt {
// CHECK-NEXT:         pub x: i32,
// CHECK-NEXT:         pub y: i32,
// CHECK-NEXT:     }
// CHECK-EMPTY:
// CHECK-NEXT:     impl Pt {
// CHECK-NEXT:         pub fn sum(&self) -> i32 {
// CHECK-NEXT:             return self.x + self.y;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn shift(&mut self, mut k: i32) {
// CHECK-NEXT:             self.x += k;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn copy_to(&self, mut out: &mut [Pt]) {
// CHECK-NEXT:             out[0usize].x = self.x;
// CHECK-NEXT:             out[0usize].y = self.y;
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn bump_self(&mut self) {
// CHECK-NEXT:             Pt::inc_x(std::slice::from_mut(self), 1);
// CHECK-NEXT:         }
// CHECK-EMPTY:
// CHECK-NEXT:         pub fn inc_x(mut p: &mut [Pt], mut k: i32) {
// CHECK-NEXT:             p[0usize].x += k;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn scale(mut p: &mut [crate::geo::Pt], mut k: i32) {
// CHECK-NEXT:     p[0usize].x *= k;
// CHECK-NEXT:     p[0usize].y *= k;
// CHECK-NEXT:     p[0usize].shift(1);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn total(mut ps: &[crate::geo::Pt], mut n: usize) -> i32 {
// CHECK-NEXT:     let mut t: i32 = 0;
// CHECK-NEXT:     for i in 0usize..n {
// CHECK-NEXT:         t += ps[i].sum();
// CHECK-NEXT:     }
// CHECK-NEXT:     return t;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn put(mut pos: &mut [usize], mut out: &mut [u8], mut b: u8) {
// CHECK-NEXT:     out[pos[0usize]] = b;
// CHECK-NEXT:     pos[0usize] = pos[0usize].wrapping_add(1usize);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn walk(mut p: &[u8], mut n: usize) -> u32 {
// CHECK-NEXT:     let mut h: u32 = 0u32;
// CHECK-NEXT:     let end_marker: &[u8] = p;
// CHECK-NEXT:     while n > 0usize {
// CHECK-NEXT:         h = h.wrapping_mul(31u32).wrapping_add(p[0usize] as u32);
// CHECK-NEXT:         p = &p[1usize..];
// CHECK-NEXT:         p = &p[1usize..];
// CHECK-NEXT:         p = &p[0usize..];
// CHECK-NEXT:         n = n.wrapping_sub(1usize);
// CHECK-NEXT:     }
// CHECK-NEXT:     return h.wrapping_add(end_marker[0usize] as u32);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn skip(mut p: &mut [i32], mut u: u32, mut n: i32) {
// CHECK-NEXT:     p = &mut p[2usize..];
// CHECK-NEXT:     p[u as usize] = 1;
// CHECK-NEXT:     for i in 0..n {
// CHECK-NEXT:         p[i as usize] += i;
// CHECK-NEXT:     }
// CHECK-NEXT:     p[0usize] = 7;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn grow(mut v: &mut [Vec<i32>], mut s: &mut [String]) {
// CHECK-NEXT:     let n: i32 = v[0usize].len() as i32;
// CHECK-NEXT:     v[0usize].push(n);
// CHECK-NEXT:     v[0usize][0usize] = 3;
// CHECK-NEXT:     bx::push_byte(&mut s[0usize], 33i8);
// CHECK-NEXT:     print!("{} {}\n", s[0usize], s[0usize].len());
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut a: crate::geo::Pt = crate::geo::Pt { x: 1, y: 2 };
// CHECK-NEXT:     scale(std::slice::from_mut(&mut a), 3);
// CHECK-NEXT:     let mut ps: Vec<crate::geo::Pt> = vec![a.clone(), crate::geo::Pt { x: 4, y: 5 }];
// CHECK-NEXT:     let mut b: crate::geo::Pt = crate::geo::Pt { x: 0, y: 0 };
// CHECK-NEXT:     ps[1usize].copy_to(std::slice::from_mut(&mut b));
// CHECK-NEXT:     b.bump_self();
// CHECK-NEXT:     let mut buf: Vec<u8> = vec![0u8; 4usize];
// CHECK-NEXT:     let mut pos: usize = 1usize;
// CHECK-NEXT:     put(std::slice::from_mut(&mut pos), &mut buf[..], 9u8);
// CHECK-NEXT:     put(std::slice::from_mut(&mut pos), &mut buf[0usize..], 8u8);
// CHECK-NEXT:     let mut v: Vec<i32> = vec![5, 6, 7, 8, 9, 10];
// CHECK-NEXT:     skip(&mut v[1usize..], 1u32, 2);
// CHECK-NEXT:     let mut s: String = String::from("hi");
// CHECK-NEXT:     grow(std::slice::from_mut(&mut v), std::slice::from_mut(&mut s));
// CHECK-NEXT:     print!("{} {} {} {} {}\n", total(&ps[..], ps.len()), total(&ps[1usize..], 1usize), b.x, pos, walk(&buf[1usize..], 1usize));
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: mod bx {
// CHECK-NEXT:     pub fn push_byte(s: &mut String, c: i8) {
// CHECK-NEXT:         let b: u8 = c as u8;
// CHECK-NEXT:         assert!(b < 128, "bx::push_byte: a byte >= 0x80 would break the String ASCII invariant");
// CHECK-NEXT:         s.push(b as char);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <string>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: namespace geo {
// CPP-EMPTY:
// CPP-NEXT: struct Pt {
// CPP-NEXT:     int x;
// CPP-NEXT:     int y;
// CPP-NEXT:     int sum() const;
// CPP-NEXT:     void shift(int k);
// CPP-NEXT:     void copy_to(Pt* out) const;
// CPP-NEXT:     void bump_self();
// CPP-NEXT:     static void inc_x(Pt* p, int k);
// CPP-NEXT: };
// CPP-EMPTY:
// CPP-NEXT: }  // namespace geo
// CPP-EMPTY:
// CPP-NEXT: namespace geo {
// CPP-EMPTY:
// CPP-NEXT: int Pt::sum() const {
// CPP-NEXT:     return this->x + this->y;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Pt::shift(int k) {
// CPP-NEXT:     this->x += k;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Pt::copy_to(Pt* out) const {
// CPP-NEXT:     out->x = this->x;
// CPP-NEXT:     out->y = this->y;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Pt::bump_self() {
// CPP-NEXT:     Pt::inc_x(this, 1);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Pt::inc_x(Pt* p, int k) {
// CPP-NEXT:     p->x += k;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace geo
// CPP-EMPTY:
// CPP-NEXT: void scale(::geo::Pt* p, int k) {
// CPP-NEXT:     p->x *= k;
// CPP-NEXT:     p->y *= k;
// CPP-NEXT:     p->shift(1);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int total(const ::geo::Pt* ps, std::size_t n) {
// CPP-NEXT:     int t = 0;
// CPP-NEXT:     for (std::size_t i = static_cast<std::size_t>(0); i < n; ++i) {
// CPP-NEXT:         t += ps[i].sum();
// CPP-NEXT:     }
// CPP-NEXT:     return t;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void put(std::size_t* pos, unsigned char* out, unsigned char b) {
// CPP-NEXT:     out[*pos] = b;
// CPP-NEXT:     ++*pos;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: unsigned walk(const unsigned char* p, std::size_t n) {
// CPP-NEXT:     unsigned h = 0U;
// CPP-NEXT:     const unsigned char* const end_marker = p;
// CPP-NEXT:     while (n > static_cast<std::size_t>(0)) {
// CPP-NEXT:         h = (h * 31U) + static_cast<unsigned>(*p);
// CPP-NEXT:         ++p;
// CPP-NEXT:         ++p;
// CPP-NEXT:         p += 0;
// CPP-NEXT:         --n;
// CPP-NEXT:     }
// CPP-NEXT:     return h + static_cast<unsigned>(*end_marker);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void skip(int* p, unsigned u, int n) {
// CPP-NEXT:     p += 2;
// CPP-NEXT:     p[static_cast<std::size_t>(u)] = 1;
// CPP-NEXT:     for (int i = 0; i < n; ++i) {
// CPP-NEXT:         p[static_cast<std::size_t>(i)] += i;
// CPP-NEXT:     }
// CPP-NEXT:     *p = 7;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void grow(std::vector<int>* v, std::string* s) {
// CPP-NEXT:     const int n = static_cast<int>(v->size());
// CPP-NEXT:     v->push_back(n);
// CPP-NEXT:     (*v)[static_cast<std::size_t>(0)] = 3;
// CPP-NEXT:     s->push_back('!');
// CPP-NEXT:     std::printf("%s %zu\n", s->c_str(), s->size());
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     ::geo::Pt a = {1, 2};
// CPP-NEXT:     scale(&a, 3);
// CPP-NEXT:     std::vector<::geo::Pt> ps = {a, ::geo::Pt{4, 5}};
// CPP-NEXT:     ::geo::Pt b = {0, 0};
// CPP-NEXT:     ps[static_cast<std::size_t>(1)].copy_to(&b);
// CPP-NEXT:     b.bump_self();
// CPP-NEXT:     std::vector<unsigned char> buf(static_cast<std::size_t>(4), static_cast<unsigned char>(0));
// CPP-NEXT:     std::size_t pos = static_cast<std::size_t>(1);
// CPP-NEXT:     put(&pos, buf.data(), static_cast<unsigned char>(9));
// CPP-NEXT:     put(&pos, buf.data() + 0, static_cast<unsigned char>(8));
// CPP-NEXT:     std::vector<int> v = {5, 6, 7, 8, 9, 10};
// CPP-NEXT:     skip(v.data() + 1, 1U, 2);
// CPP-NEXT:     std::string s = "hi";
// CPP-NEXT:     grow(&v, &s);
// CPP-NEXT:     std::printf("%d %d %d %zu %u\n", total(ps.data(), ps.size()), total(ps.data() + 1, static_cast<std::size_t>(1)), b.x, pos, walk(buf.data() + 1, static_cast<std::size_t>(1)));
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace geo {
struct Pt {
    int x;
    int y;
    int sum() const { return x + y; }
    void shift(int k) { x += k; }
    void copy_to(Pt* out) const { out->x = x; out->y = y; }
    void bump_self() { inc_x(this, 1); }
    static void inc_x(Pt* p, int k) { p->x = p->x + k; }
};
}  // namespace geo

void scale(geo::Pt* p, int k) {
    p->x *= k;
    (*p).y = (*p).y * k;
    p[0].shift(1);
}

int total(const geo::Pt* ps, std::size_t n) {
    int t = 0;
    for (std::size_t i = 0; i < n; ++i) {
        t += ps[i].sum();
    }
    return t;
}

void put(std::size_t* pos, std::uint8_t* out, std::uint8_t b) {
    out[*pos] = b;
    ++*pos;
}

unsigned walk(const unsigned char* p, std::size_t n) {
    unsigned h = 0;
    const unsigned char* const end_marker = p;
    while (n > 0) {
        h = h * 31 + *p;
        ++p;
        p++;
        p = p + 0;
        n -= 1;
    }
    return h + end_marker[0];
}

void skip(int* p, unsigned u, int n) {
    p += 2;
    p[u] = 1;
    for (int i = 0; i < n; ++i) {
        p[i] = p[i] + i;
    }
    p[0] = 7;
}

void grow(std::vector<int>* v, std::string* s) {
    const int n = static_cast<int>(v->size());
    v->push_back(n);
    (*v)[0] = 3;
    s->push_back('!');
    std::printf("%s %zu\n", s->c_str(), s->size());
}

int main() {
    geo::Pt a = {1, 2};
    scale(&a, 3);
    std::vector<geo::Pt> ps = {a, {4, 5}};
    geo::Pt b = {0, 0};
    ps[1].copy_to(&b);
    b.bump_self();
    std::vector<std::uint8_t> buf(4, 0);
    std::size_t pos = 1;
    put(&pos, buf.data(), 9);
    put(&pos, &buf[0], 8);
    std::vector<int> v = {5, 6, 7, 8, 9, 10};
    skip(v.data() + 1, 1U, 2);
    std::string s = "hi";
    grow(&v, &s);
    std::printf("%d %d %d %zu %u\n", total(ps.data(), ps.size()), total(&ps[1], 1), b.x, pos,
                walk(buf.data() + 1, 1));
    return 0;
}
