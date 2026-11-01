// FR-260: module visibility has ONE canonical spelling: `mod` at the root,
// `pub mod` nested (a private nested module is unreachable from the crate
// root, E0603), `pub fn` / plain `fn` inside a module, no `pub` at the root,
// no restricted `pub(..)`. A private nested module is refused, located.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
mod a {
    // CHECK: ns-rust-visibility.rs:[[@LINE+1]]:5: error: unsupported: private nested module (the subset spells it `pub mod`: a private one is unreachable from the crate root)
    mod hidden {
        pub fn f() -> i32 {
            return 1;
        }
    }
}

fn main() {
    print!("{}\n", 0);
}
