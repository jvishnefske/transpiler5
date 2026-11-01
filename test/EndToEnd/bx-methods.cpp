// REQUIRES: cargo
// FR-257: emitrust-bx C++ -> Rust, non-virtual member functions end to end
// (byte-diff vs clang++). A const member function <=> `&self`, a non-const
// one <=> `&mut self`, a static one <=> an associated fn called `S::f(..)`;
// implicit member access and `this->f` <=> `self.f`, an implicit call
// `g()` <=> `self.g()`; in-class AND out-of-line definitions
// (`int Counter::total() const {..}`); members of a struct inside a
// namespace called from outside (`::geo::Rect::unit()` <=>
// `crate::geo::Rect::unit()`); a method taking `const S&` of its own type
// (`a.dist(b)` <=> `a.dist(&b)`), returning a struct by value, mutating a
// vector member, and the receiver-alias shapes rustc 1.98's two-phase
// borrows accept: `c.add(c.total())`, `o.inner.add(o.inner.total())` and
// `this->add(this->count)` (arguments READ the receiver of a `&mut self`
// call). UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_methods > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

struct Counter {
    std::vector<int> seen;
    int count;
    std::string tag;

    int total() const;
    void add(int k) {
        seen.push_back(k);
        count += 1;
    }
    void twice() {
        this->add(this->count);
        add(total() % 97);
    }
    bool has(int k) const {
        for (const int x : seen) {
            if (x == k) {
                return true;
            }
        }
        return false;
    }
    static Counter named(const std::string& t) {
        Counter c = {{}, 0, t};
        return c;
    }
};

int Counter::total() const {
    int s = 0;
    for (const int x : this->seen) {
        s += x;
    }
    return s + count * 1000;
}

namespace geo {

struct Rect {
    long long w;
    long long h;

    long long area() const {
        return w * h;
    }
    long long dist(const Rect& o) const {
        const long long dw = w - o.w;
        const long long dh = h - o.h;
        return dw * dw + dh * dh;
    }
    void grow(long long by) {
        w += by;
        h = h * 2LL + by;
    }
    Rect rotated() const {
        return Rect{h, w};
    }
    static Rect unit() {
        return Rect{1LL, 1LL};
    }
};

}  // namespace geo

struct Outer {
    Counter inner;
    geo::Rect frame;
};

int main() {
    Counter c = Counter::named(std::string("c"));
    c.add(5);
    c.add(12);
    c.add(c.total());
    c.twice();
    std::printf("%s count=%d total=%d has12=%d has13=%d\n", c.tag.c_str(), c.count, c.total(),
                static_cast<int>(c.has(12)), static_cast<int>(c.has(13)));
    geo::Rect a = {3LL, 4LL};
    geo::Rect b = ::geo::Rect::unit();
    b.grow(6LL);
    const geo::Rect r = a.rotated();
    std::printf("area a=%lld b=%lld r=(%lld,%lld) dist=%lld\n", a.area(), b.area(), r.w, r.h, a.dist(b));
    Outer o = {Counter::named(std::string("o")), geo::Rect{7LL, 2LL}};
    o.inner.add(31);
    o.inner.add(o.inner.total());
    o.frame.grow(o.frame.area());
    std::printf("%d %d %lld %lld\n", o.inner.count, o.inner.total(), o.frame.w, o.frame.h);
    std::vector<geo::Rect> boxes = {a, b, r};
    for (geo::Rect& x : boxes) {
        x.grow(2LL);
    }
    long long sum = 0LL;
    for (const geo::Rect& x : boxes) {
        sum += x.area() - x.dist(a);
    }
    boxes[1].grow(9LL);
    std::printf("sum=%lld b1=%lld a=%lld\n", sum, boxes[1].area(), a.area());
    return 0;
}
