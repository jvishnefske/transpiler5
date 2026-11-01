// FR-260: the lookup shape where the two languages DISAGREE. C++ unqualified
// lookup searches enclosing namespaces, so `f()` inside `namespace a` finds
// ::f; a Rust path never searches enclosing modules (rustc E0425). rs_parse
// resolves with Rust's rules, so it must refuse -- located, naming the
// function C++ would have found -- rather than guess either meaning.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
fn f() -> i32 {
    return 1;
}

mod a {
    pub mod b {
        pub fn g() -> i32 {
            // CHECK: ns-rust-outer.rs:[[@LINE+1]]:20: error: unsupported: `f` is not defined in module `a::b`: a Rust path does not search enclosing modules (C++ unqualified lookup would find `f`); spell it `crate::f`
            return f();
        }
    }
}

fn main() {
    print!("{}\n", crate::a::b::g());
}
