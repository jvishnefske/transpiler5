// FR-260: C++ may reopen a namespace; Rust may not reopen a module (E0428).
// The canonical merges a reopened namespace into ONE `mod` (normal form N13),
// so a second `mod a` block in Rust input is refused, located at its name.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
mod a {
    pub fn f() -> i32 {
        return 1;
    }
}

// CHECK: ns-rust-reopen.rs:[[@LINE+1]]:5: error: unsupported: module 'a' defined twice (a Rust module cannot be reopened; the subset merges a reopened C++ namespace into one `mod`)
mod a {
    pub fn g() -> i32 {
        return 2;
    }
}

fn main() {
    print!("{}\n", crate::a::f());
}
