// REQUIRES: cargo
// FR-257: emitrust-bx C++ -> Rust, aggregate structs end to end (byte-diff vs
// clang++). A struct with public data members of Kernel types <=> a
// `#[derive(Clone)] struct` with `pub` fields; aggregate initialization
// `S s = {a, b};` / `S{a, b}` / `S s{a, b};` <=> `S { f: a, g: b }` (every
// field, declaration order -- both languages evaluate the initializers left
// to right); field reads, writes and compound writes `p.f op= e` through a
// local, an element `v[i].f`, a nested member `o.inner.f` and a by-value
// parameter; structs holding std::string, std::vector, other structs and a
// vector of structs; struct return by value (C++17 implicit move <=> a bare
// `return r;`). The observable half of value semantics: a COPY (`S t = s;`,
// `push_back(s)`, a by-value argument <=> `.clone()`) is independent of its
// original afterwards, and a MOVE (`std::move(s)` <=> `s`) leaves nothing to
// observe. Unsigned members wrap. UB-free; distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_structs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct Point {
    int x;
    long long y;
};

struct Label {
    std::string text;
    unsigned hits;
    bool shown;
};

struct Shape {
    Point origin;
    Label name;
    std::vector<Point> corners;
    double scale;
};

Point shifted(Point p, int dx) {
    p.x += dx;
    p.y = p.y * 3LL - 1LL;
    return p;
}

long long area2(Shape s) {
    long long acc = 0LL;
    for (std::size_t i = 0; i < s.corners.size(); ++i) {
        const Point a = s.corners[i];
        const Point b = s.corners[(i + 1) % s.corners.size()];
        acc += static_cast<long long>(a.x) * b.y - static_cast<long long>(b.x) * a.y;
    }
    return acc;
}

Shape make_shape(int k) {
    Shape s = {{k, static_cast<long long>(k) * 2LL}, {std::string("sh"), 0U, false}, {}, 1.5};
    s.corners.push_back(Point{0, 0LL});
    s.corners.push_back(Point{k, 0LL});
    s.corners.push_back(Point{k, static_cast<long long>(k + 1)});
    s.name.text += std::to_string(k);
    return s;
}

void show(const Shape s) {
    std::printf("%s o=(%d,%lld) n=%zu hits=%u shown=%d scale=%.3f\n", s.name.text.c_str(), s.origin.x,
                s.origin.y, s.corners.size(), s.name.hits, static_cast<int>(s.name.shown), s.scale);
}

int main() {
    Point p = {3, -7LL};
    Point q{11, 13LL};
    Point r = shifted(p, 5);
    std::printf("p=(%d,%lld) q=(%d,%lld) r=(%d,%lld)\n", p.x, p.y, q.x, q.y, r.x, r.y);
    Point copy = q;
    copy.x = 99;
    std::printf("copy is independent: %d %d\n", q.x, copy.x);
    Shape s = make_shape(4);
    Shape t = s;
    t.corners[1].x = 40;
    t.name.text += "-t";
    t.name.hits -= 1U;
    t.scale *= 2.0;
    t.origin.y += 1000LL;
    show(s);
    show(t);
    std::printf("area2 s=%lld t=%lld\n", area2(s), area2(t));
    std::vector<Shape> all = {s, make_shape(6)};
    all.push_back(t);
    all[2].name.shown = !all[2].name.shown;
    Shape moved = std::move(t);
    moved.corners.push_back(Point{-2, -3LL});
    all.push_back(std::move(moved));
    long long total = 0LL;
    for (const Shape& x : all) {
        total += area2(x) + x.origin.y;
        std::printf("%s:%zu ", x.name.text.c_str(), x.corners.size());
    }
    std::printf("\ntotal=%lld\n", total);
    for (Shape& x : all) {
        x.name.hits += 7U;
        x.origin.x -= 1;
    }
    std::printf("%u %d %u\n", all[0].name.hits, all[3].origin.x, s.name.hits);
    return 0;
}
