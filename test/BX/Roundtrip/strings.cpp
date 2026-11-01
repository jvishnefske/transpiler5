// FR-251: the std::string rows, C++ -> canonical Rust under
// --check-roundtrip (laws 1-2 asserted on BOTH sides), plus the canonical
// C++ (CPP). Pins `std::string s;` / `= "lit"` <=> String::new() /
// String::from("lit"); `a + "d"` (an lvalue lhs is COPIED: `a.clone() +
// "d"`), a chained `"<" + a + b + ">"` (a literal lhs becomes
// `String::from("<")`, the temporaries chain without clones); `+=` of a
// string / literal / char and push_back(c) <=> push_str(..) /
// bx::push_byte(&mut s, c); read-only `s[i]` <=> `s.as_bytes()[i] as i8`
// (char is SIGNED); substr(p, n) / substr(p) <=> bx::substr /
// bx::substr_from; length() == size() <=> len(); `<` and `== "abc"`;
// std::to_string <=> .to_string(); printf %s of `.c_str()` (also of a
// temporary) <=> `{}`; copy-assignment `e = a;` <=> `e = a.clone();`;
// `const std::string&` <=> `&String`, with the call site `greet(&c, 7)`.
// RUN: %emitrust-bx --check-roundtrip %s | FileCheck %s
// RUN: %emitrust-bx --emit=cpp %s | FileCheck --check-prefix=CPP %s
// CHECK:      fn greet(name: &String, mut n: i32) -> String {
// CHECK-NEXT:     let mut s: String = String::from("hi ");
// CHECK-NEXT:     s.push_str(name);
// CHECK-NEXT:     s.push_str("!");
// CHECK-NEXT:     bx::push_byte(&mut s, 63i8);
// CHECK-NEXT:     bx::push_byte(&mut s, 35i8);
// CHECK-NEXT:     return s.clone() + &n.to_string();
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut e: String = String::new();
// CHECK-NEXT:     let mut a: String = String::from("abc");
// CHECK-NEXT:     let mut b: String = a.clone() + "d";
// CHECK-NEXT:     let mut c: String = String::from("<") + &a + &b + ">";
// CHECK-NEXT:     let mut ch: i8 = c.as_bytes()[1usize] as i8;
// CHECK-NEXT:     let mut sub: String = bx::substr(&c, 1usize, 3usize);
// CHECK-NEXT:     let mut tail: String = bx::substr_from(&c, 4usize);
// CHECK-NEXT:     let mut lt: bool = a < b;
// CHECK-NEXT:     let mut eq: bool = a == "abc";
// CHECK-NEXT:     let mut n: usize = c.len().wrapping_add(c.len()).wrapping_add((-5i64).to_string().len());
// CHECK-NEXT:     print!("{} {} {} {} {} {} {}\n", greet(&c, 7), sub, ch as i32, lt as i32, eq as i32, n, e.is_empty() as i32);
// CHECK-NEXT:     print!("{} {}\n", tail, a.clone() + &b);
// CHECK-NEXT:     e = a.clone();
// CHECK-NEXT:     e = b.clone() + "!";
// CHECK-NEXT:     e.clear();
// CHECK-NEXT: }
// CPP:      #include <cstdio>
// CPP-NEXT: #include <string>
// CPP-EMPTY:
// CPP-NEXT: std::string greet(const std::string& name, int n) {
// CPP-NEXT:     std::string s = "hi ";
// CPP-NEXT:     s += name;
// CPP-NEXT:     s += "!";
// CPP-NEXT:     s.push_back('?');
// CPP-NEXT:     s.push_back('#');
// CPP-NEXT:     return s + std::to_string(n);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::string e;
// CPP-NEXT:     std::string a = "abc";
// CPP-NEXT:     std::string b = a + "d";
// CPP-NEXT:     std::string c = std::string("<") + a + b + ">";
// CPP-NEXT:     char ch = c[static_cast<std::size_t>(1)];
// CPP-NEXT:     std::string sub = c.substr(static_cast<std::size_t>(1), static_cast<std::size_t>(3));
// CPP-NEXT:     std::string tail = c.substr(static_cast<std::size_t>(4));
// CPP-NEXT:     bool lt = a < b;
// CPP-NEXT:     bool eq = a == "abc";
// CPP-NEXT:     std::size_t n = c.size() + c.size() + std::to_string(-5LL).size();
// CPP-NEXT:     std::printf("%s %s %d %d %d %zu %d\n", greet(c, 7).c_str(), sub.c_str(), static_cast<int>(ch), static_cast<int>(lt), static_cast<int>(eq), n, static_cast<int>(e.empty()));
// CPP-NEXT:     std::printf("%s %s\n", tail.c_str(), (a + b).c_str());
// CPP-NEXT:     e = a;
// CPP-NEXT:     e = b + "!";
// CPP-NEXT:     e.clear();
// CPP-NEXT:     return 0;
// CPP-NEXT: }
#include <cstdio>
#include <string>

std::string greet(const std::string& name, int n) {
    std::string s = "hi ";
    s += name;
    s += "!";
    s += '?';
    s.push_back('#');
    return s + std::to_string(n);
}

int main() {
    std::string e;
    std::string a = "abc";
    std::string b = a + "d";
    std::string c = "<" + a + b + ">";
    char ch = c[1];
    std::string sub = c.substr(1, 3);
    std::string tail = c.substr(4);
    bool lt = a < b;
    bool eq = a == "abc";
    std::size_t n = c.size() + c.length() + std::to_string(-5LL).size();
    std::printf("%s %s %d %d %d %zu %d\n", greet(c, 7).c_str(), sub.c_str(), ch, lt, eq, n, e.empty());
    std::printf("%s %s\n", tail.c_str(), (a + b).c_str());
    e = a;
    e = b + "!";
    e.clear();
    return 0;
}
