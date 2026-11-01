// REQUIRES: cargo
// FR-251: emitrust-bx C++ -> Rust, the std::string rows end to end (byte-diff
// vs clang++). Pins `std::string s;` / `= "lit"` <=> String::new /
// String::from, `a + b` (lvalue lhs copies: `a.clone() + &b`; a temporary
// lhs chains: `a.clone() + &b + "c"`; a literal lhs: `String::from("x") +
// &a`), `+=` of a string, a literal and a char (<=> push_str /
// bx::push_byte), push_back(c), size()/length()/empty(), read-only `s[i]`
// (char is SIGNED: `s.as_bytes()[i] as i8`), substr(p, n) and substr(p)
// INCLUDING the C++ clamp of n to size() - p, every comparison operator
// (std::string compares as unsigned bytes, Rust String byte-
// lexicographically: identical under the subset's ASCII invariant),
// std::to_string of every integer type including negatives, and printf
// `%s` of `s.c_str()`. UB-free: every index and substr position is in
// range. Distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_strings > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += s;
    }
    return out;
}

int digit_sum(const std::string& s) {
    int sum = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            sum += c - '0';
        }
    }
    return sum;
}

std::string caesar(const std::string& s, int shift) {
    std::string out;
    for (std::size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>('a' + (c - 'a' + shift) % 26);
        }
        out.push_back(c);
    }
    return out;
}

void cmp(const std::string& a, const std::string& b) {
    std::printf("%s ? %s : %d %d %d %d %d %d\n", a.c_str(), b.c_str(), a == b, a != b, a < b, a <= b, a > b,
                a >= b);
}

int main() {
    std::string empty;
    std::string hello = "hello";
    std::string world = "world";
    std::printf("[%s] %zu %d\n", empty.c_str(), empty.size(), empty.empty());
    std::string hw = hello + ", " + world;
    std::printf("%s %zu\n", hw.c_str(), hw.length());
    std::string tagged = "<" + world + ">";
    std::printf("%s\n", tagged.c_str());
    hw += "!";
    hw += world;
    hw += '?';
    hw.push_back('#');
    std::printf("%s %zu %d\n", hw.c_str(), hw.size(), hw.empty());
    std::printf("%s|%s|%s|%s\n", hw.substr(0, 5).c_str(), hw.substr(7, 3).c_str(), hw.substr(13).c_str(),
                hw.substr(10, 400).c_str());
    std::printf("%s|%s\n", hw.substr(hw.size()).c_str(), hw.substr(2, 0).c_str());
    std::printf("%d %d %d\n", hw[0], hw[4], hw[hw.size() - 1]);
    cmp(hello, world);
    cmp(world, hello);
    cmp(hello, hello);
    cmp(hello, "help");
    cmp("hell", hello);
    cmp(empty, "a");
    std::printf("%d %d\n", hello == "hello", world != "world");
    std::string nums = std::to_string(-42) + "/" + std::to_string(9000000000LL) + "/" +
                       std::to_string(4000000000U) + "/" + std::to_string(18446744073709551615ULL) + "/" +
                       std::to_string(hw.size());
    std::printf("%s ds=%d\n", nums.c_str(), digit_sum(nums));
    std::printf("%s|%s\n", repeat("ab", 3).c_str(), repeat(world, 0).c_str());
    std::printf("%s %s\n", caesar("hello, world", 3).c_str(), caesar(hello, 13).c_str());
    std::string acc = "x";
    for (int i = 0; i < 4; ++i) {
        acc = acc + std::to_string(i * i) + acc;
    }
    std::printf("%s %zu\n", acc.c_str(), acc.size());
    acc.clear();
    std::printf("[%s] %d\n", acc.c_str(), acc.empty());
    return 0;
}
