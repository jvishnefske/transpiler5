// FR-260: inside a module, `bx::chr` resolves RELATIVE to the module (rustc
// E0433), so the canonical spells the safe helpers `crate::bx::..` there.
// Rust input with a bare `bx::` inside a module is refused, located -- never
// accepted as if it named the root helper.
// RUN: not %emitrust-bx %s 2>&1 | FileCheck %s
mod a {
    pub fn show(c: i32) {
        // CHECK: ns-rust-bx.rs:[[@LINE+1]]:24: error: unsupported: `bx::` inside a module resolves relative to it (the subset spells it `crate::bx::`)
        print!("{}\n", bx::chr(c));
    }
}

fn main() {
    crate::a::show(65);
}
