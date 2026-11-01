// REQUIRES: cargo
// FR-257: emitrust-bx C++ -> Rust, structs through references end to end
// (byte-diff vs clang++). `const S&` / `S&` parameters <=> `&S` / `&mut S`
// (field reads `r.f`, writes `r.f = e` through the reference, member calls on
// it); `f(s.inner)` / `f(v[i])` / `f(*this)` bind members, elements and the
// object itself; a struct holding a vector of ITSELF (a tree: legal in both
// languages, the recursion goes through the vector); range-for over a vector
// of structs by value / const& / & (`.iter().cloned()` / `.iter()` /
// `.iter_mut()`); copy assignment `a = b;` <=> `a = b.clone();` and
// `*this = other;` <=> `*self = other.clone();`; a counting loop bounded by
// `t.kids.size()` whose body writes elements only (no resize). UB-free;
// distinct values at every call site.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_struct_refs > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
#include <cstdio>
#include <string>
#include <vector>

struct Tree {
    int value;
    std::vector<Tree> kids;

    int sum() const {
        int s = value;
        for (const Tree& k : kids) {
            s += k.sum();
        }
        return s;
    }
    void adopt(const Tree& t) {
        kids.push_back(t);
    }
    void reset(const Tree& other) {
        *this = other;
    }
};

struct Account {
    std::string owner;
    long long balance;
    unsigned ops;
};

void deposit(Account& a, long long amount) {
    a.balance += amount;
    a.ops += 1U;
}

long long worth(const Account& a) {
    return a.balance * 2LL + static_cast<long long>(a.ops);
}

void bump_all(Tree& t, int by) {
    t.value += by;
    for (Tree& k : t.kids) {
        bump_all(k, by + 1);
    }
}

int depth(const Tree& t) {
    int best = 0;
    for (std::size_t i = 0; i < t.kids.size(); ++i) {
        const int d = depth(t.kids[i]);
        if (d > best) {
            best = d;
        }
    }
    return best + 1;
}

int main() {
    Tree leaf = {3, {}};
    Tree mid = {10, {}};
    mid.adopt(leaf);
    mid.adopt(Tree{4, {}});
    Tree root = {100, {}};
    root.adopt(mid);
    root.adopt(leaf);
    std::printf("sum=%d depth=%d kids=%zu\n", root.sum(), depth(root), root.kids.size());
    bump_all(root, 1);
    bump_all(root.kids[0], 10);
    std::printf("sum=%d leaf=%d k0=%d\n", root.sum(), leaf.value, root.kids[0].value);
    for (std::size_t i = 0; i < root.kids.size(); ++i) {
        root.kids[i].value = root.kids[i].value * 2 + static_cast<int>(i);
    }
    std::printf("after loop sum=%d\n", root.sum());
    Tree spare = {7, {}};
    spare.reset(mid);
    std::printf("spare=%d/%zu mid=%d\n", spare.value, spare.kids.size(), mid.value);
    Account acc = {std::string("ann"), 50LL, 0U};
    deposit(acc, 25LL);
    deposit(acc, -5LL);
    std::vector<Account> book = {acc, Account{std::string("bob"), 7LL, 3U}};
    deposit(book[1], 1000LL);
    Account copy = book[0];
    copy.owner += "-copy";
    deposit(copy, 1LL);
    long long all = 0LL;
    for (Account a : book) {
        a.balance += 1LL;
        all += worth(a);
    }
    for (Account& a : book) {
        a.ops += 10U;
    }
    Account last = acc;
    last = book[1];
    std::printf("%s %lld %u | %s %lld | all=%lld | %s %u\n", acc.owner.c_str(), acc.balance, acc.ops,
                copy.owner.c_str(), worth(copy), all, last.owner.c_str(), book[0].ops);
    return 0;
}
