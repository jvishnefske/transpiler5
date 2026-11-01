// FR-260: a private `fn` in a module is a C++ anonymous-namespace member; Rust
// lets only that module and its descendants reach it (E0603). The checker
// enforces the same rule on BOTH inputs, so a sibling module's reference is
// a located refusal; the descendant's reference is accepted.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
mod a {
    fn secret() -> i32 {
        return 7;
    }

    pub mod inner {
        pub fn ok() -> i32 {
            return crate::a::secret();
        }
    }
}

mod b {
    pub fn peek() -> i32 {
        // CHECK: ns-rust-private.rs:[[@LINE+1]]:16: error: unsupported: 'a::secret' is internal to namespace 'a' (a C++ anonymous-namespace member, a private Rust `fn`) and is referenced from outside it
        return crate::a::secret();
    }
}

fn main() {
    print!("{} {}\n", crate::a::inner::ok(), crate::b::peek());
}
// CHECK-NOT: error
