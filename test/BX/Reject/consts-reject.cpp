// FR-264: the namespace-scope constants with no 1:1 Rust spelling are
// LOCATED refusals, never a crate that fails to build or computes something
// else. Pinned, sorted by location with the exact count: a mutable, extern,
// volatile or thread_local global keeps "global variable" (only a const
// scalar is a constant); a constant of a non-kernel type (array, pointer,
// float) and a non-constant initializer (a call; clang's JSON carries no
// evaluated value, so constness is decided structurally); `&kOk` keeps the
// address-of refusal (a constant is no place); ANY binding of a constant's
// name in its namespace -- local, parameter, loop variable, lambda parameter
// -- because Rust treats that name as a PATTERN (rustc E0005/E0530 even for
// an equal value); an anonymous-namespace constant referenced from outside
// (rustc E0603); and a constant whose definition N13's namespace merge moves
// after its use (C++ defines a constant before its use).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}15{{$}}
#include <cstdint>
#include <cstdio>

// CHECK: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: global variable
int counter = 3;
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: global variable
extern const int kExtern;
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: global variable
const volatile int kVolatile = 1;
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: global variable
thread_local const int kTls = 2;
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: constant of non-kernel type 'const std::uint8_t[3]'
static const std::uint8_t kTable[] = {1, 2, 3};
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: constant of non-kernel type 'const int *const'
const int* const kPtr = &counter;
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:1: error: unsupported: constant of non-kernel type 'const float'
const float kHalf = 0.5f;

int Four() {
    return 4;
}
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:19: error: unsupported: constant initializer is not a constant expression
const int kCall = Four();
const int kOk = 5;

int First(const int* p) {
    return p[0];
}

int AddressOf() {
    // CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:18: error: unsupported: address of something other than a variable or data member
    return First(&kOk);
}

int Local(int x) {
    // CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:5: error: unsupported: variable 'kOk' has the name of a constant
    int kOk = 2;
    return x + kOk;
}

// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:15: error: unsupported: variable 'kOk' has the name of a constant
int Param(int kOk) {
    return kOk;
}

int Loop(int x) {
    // CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:5: error: unsupported: variable 'kOk' has the name of a constant
    for (int kOk = 0; kOk < 2; ++kOk) {
        x += kOk;
    }
    // CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:21: error: unsupported: variable 'kOk' has the name of a constant
    auto f = [](int kOk) -> int { return kOk; };
    return f(x);
}

namespace geo {
namespace {
const int kHidden = 9;
}
const int k1 = 1;
}  // namespace geo
const int k0 = 2;
namespace geo {
// CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:16: error: unsupported: constant 'k0' used before its definition (C++ defines a constant before its use)
const int k3 = k0 + k1;
}  // namespace geo

int main() {
    // CHECK-NEXT: consts-reject.cpp:[[@LINE+1]]:28: error: unsupported: 'geo::kHidden' is internal to namespace 'geo' (a C++ anonymous-namespace member, a private Rust `const`) and is referenced from outside it
    std::printf("%d %d\n", geo::kHidden, geo::k3);
    return 0;
}
// CHECK-NOT: error
