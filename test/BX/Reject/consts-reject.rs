// FR-264: Rust constants with no 1:1 C++ spelling are LOCATED refusals.
// Rust accepts a forward reference between constants (C++ defines a
// constant before its use, and the subset keeps definition order); a binding
// of a constant's name is a rustc error anyway (E0005/E0530: the name is a
// pattern) and is refused rather than renamed; a duplicate constant, a
// constant named like a function or a namespace of the same scope (C++
// redefinition), a non-kernel type, an initializer that is not a constant
// expression (a call), and a private module constant referenced from outside
// its module (rustc E0603).
// RUN: not %emitrust-bx --emit=cpp %s -o %t.cpp 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.cpp
// COUNT: {{^}}9{{$}}
#![allow(non_upper_case_globals)]

// CHECK: consts-reject.rs:[[@LINE+1]]:21: error: constant 'kLater' used before its definition (C++ defines a constant before its use)
const kEarly: i32 = kLater + 1;

const kLater: i32 = 2;

const kDup: i32 = 1;

// CHECK: consts-reject.rs:[[@LINE+1]]:7: error: constant 'kDup' defined twice
const kDup: i32 = 2;

// CHECK: consts-reject.rs:[[@LINE+1]]:7: error: constant 'helper' has the name of a function
const helper: i32 = 3;

// CHECK: consts-reject.rs:[[@LINE+1]]:7: error: constant 'geo' has the name of a namespace declared in the same scope
const geo: i32 = 4;

// CHECK: consts-reject.rs:[[@LINE+1]]:7: error: constant of non-kernel type 'String'
const kName: String = String::new();

// CHECK: consts-reject.rs:[[@LINE+1]]:20: error: constant initializer is not a constant expression
const kCall: i32 = helper();

fn helper() -> i32 {
    return 1;
}

mod geo {
    const kHidden: i32 = 9;

    pub fn get() -> i32 {
        return kHidden;
    }
}

fn shadow(x: i32) -> i32 {
    // CHECK: consts-reject.rs:[[@LINE+1]]:5: error: variable 'kLater' has the name of a constant
    let kLater: i32 = 1;
    return x + kLater;
}

// CHECK: consts-reject.rs:[[@LINE+1]]:10: error: variable 'kEarly' has the name of a constant
fn param(kEarly: i32) -> i32 {
    return kEarly;
}

fn main() {
    // CHECK: consts-reject.rs:[[@LINE+1]]:20: error: unsupported: 'geo::kHidden' is internal to namespace 'geo' (a C++ anonymous-namespace member, a private Rust `const`) and is referenced from outside it
    print!("{}\n", crate::geo::kHidden + shadow(1) + param(2) + crate::geo::get());
}
