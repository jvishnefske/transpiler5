// REQUIRES: cargo
// FR-260: emitrust-bx C++ -> Rust, namespaces end to end (byte-diff vs
// clang++). Pins every lookup shape the canonical spells: a bare call that
// C++ resolves to the INNERMOST declaration (inner hides outer: `scale`
// inside shapes::solid is shapes::solid::scale, not shapes::scale or
// ::scale), sibling namespaces defining the same name (`shapes::area` vs
// `units::area`), qualified `shapes::scale`, global-qualified `::scale`,
// and calls from the root into three-deep `shapes::solid::deep::cube`.
// Rust spells each `f` / `crate::a::f`; values differ at every call site
// so a mis-resolved call changes the output. UB-free.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_namespaces > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

int scale(int x) {
    return x + 1;
}

namespace shapes {

int scale(int x) {
    return x * 10;
}

int area(int w, int h) {
    return scale(w) * h;
}

namespace solid {

int scale(int x) {
    return x * 100;
}

int volume(int w, int h, int d) {
    return scale(w) + shapes::scale(h) + ::scale(d);
}

namespace deep {

long long cube(int x) {
    long long v = static_cast<long long>(x);
    return v * v * v + static_cast<long long>(solid::scale(x));
}

}  // namespace deep

}  // namespace solid

}  // namespace shapes

namespace units {

int area(int w, int h) {
    return w * h - 3;
}

std::string label(const std::string& name, int n) {
    std::string out = name;
    out += ":";
    out += std::to_string(n);
    return out;
}

int total(const std::vector<int>& v) {
    int s = 0;
    for (int x : v) {
        s += shapes::area(x, 2) - area(x, 2);
    }
    return s;
}

}  // namespace units

int main() {
    std::printf("%d %d %d\n", scale(4), shapes::scale(5), shapes::solid::scale(6));
    std::printf("%d %d\n", shapes::area(7, 3), units::area(7, 3));
    std::printf("%d %d\n", shapes::solid::volume(2, 3, 4), shapes::solid::volume(9, 8, 7));
    std::printf("%lld %lld\n", shapes::solid::deep::cube(11), ::shapes::solid::deep::cube(-12));
    std::vector<int> v = {1, 5, 9};
    std::printf("%d %s\n", units::total(v), units::label(std::string("pts"), units::total(v) + 13).c_str());
    return 0;
}
