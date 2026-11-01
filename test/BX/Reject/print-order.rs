// FR-263, the Rust side: Rust evaluates operands left to right, so this
// program has ONE output -- but its C++ spelling would have two (g++ evaluates
// these printf arguments right to left). Printing the C++ would hand a reader
// a program whose behaviour depends on the compiler; the subset refuses the
// statement instead, located at the second printing call. Same rule, same
// wording as the C++ side (print-order.cpp).
// RUN: not %emitrust-bx --from=rust --emit=cpp %s -o %t.cpp 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// COUNT: {{^}}2{{$}}
fn g(x: i32) -> i32 {
    print!("g{} ", x);
    return x;
}

fn main() {
    // CHECK: print-order.rs:[[@LINE+1]]:25: error: unsupported: two calls that may print in one statement without a sequence point between them (C++ leaves their order unspecified; normalizer: evaluate each into a local first)
    let t: i32 = g(3) - g(4);
    // CHECK: print-order.rs:[[@LINE+1]]:32: error: unsupported: two calls that may print
    print!("{} {} {}\n", g(5), g(6), t);
}
