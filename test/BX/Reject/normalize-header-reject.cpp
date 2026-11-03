// FR-254 R4's refusals under --normalize --project-root: an imported
// project-header function is judged by the ORDINARY rules, and each of its
// residuals is LOCATED IN THE HEADER (file:line:col of the header, never the
// main file's coordinates); every call of a header function with a residual
// is refused AT THE CALL SITE ("call of a rejected header function"), so a
// per-function census never counts the caller clean -- transitively
// (`Outer` -> `Inner`). Pinned: a static local in a header function (per-TU
// state); two REACHABLE overloads (the subset has one function per name;
// importing either would let a call bind the other); a header function named
// like a main-file function (an overload across files -- binding the call by
// name would SILENTLY call the main-file one); a constexpr function (the
// importer's own wording, now in the header); a function template stays a
// library call (not eligible); `Plus1` imports clean, so its caller has no
// error; `Unreached` has a residual but nothing reaches it, so it is never
// imported and never reported (the import is lazy). The OFF run pins that
// without --project-root every header call keeps "call to library function".
// RUN: not %emitrust-bx --normalize --project-root=%S %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error: unsupported:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not grep -q Unreached %t.err
// RUN: not test -e %t.rs
// RUN: not %emitrust-bx --normalize %s 2>&1 | FileCheck --check-prefix=OFF %s
// COUNT: {{^}}16{{$}}
#include <cstdio>

#include "Inputs/normalize-header-reject.h"

double Scale(double v) {
    return v * 2.0;
}

int UsesCounter() {
    return Counter();
}

int UsesStore() {
    return Store16(1) + Store16(1, 2);
}

int UsesScale() {
    return Scale(2);
}

int UsesOuter() {
    return Outer(3);
}

int UsesTwice() {
    return Twice(4);
}

int UsesId() {
    return Id(5);
}

int UsesOk() {
    return Plus1(6);
}

int main() {
    std::printf("%d %d %d %d %d %d %d %f\n", UsesCounter(), UsesStore(), UsesScale(), UsesOuter(), UsesTwice(),
                UsesId(), UsesOk(), Scale(1.5));
    return 0;
}

// Main-file call sites first (sorted by file, then position), each naming the
// header function; `UsesOk` (line 57, calling the clean `Plus1`) has none.
// CHECK: normalize-header-reject.cpp:33:12: error: unsupported: call of a rejected header function 'Counter'
// CHECK-NEXT: normalize-header-reject.cpp:37:12: error: unsupported: call of a rejected header function 'Store16'
// CHECK-NEXT: normalize-header-reject.cpp:37:25: error: unsupported: call of a rejected header function 'Store16'
// CHECK-NEXT: normalize-header-reject.cpp:41:12: error: unsupported: call of a rejected header function 'Scale'
// CHECK-NEXT: normalize-header-reject.cpp:45:12: error: unsupported: call of a rejected header function 'Outer'
// CHECK-NEXT: normalize-header-reject.cpp:49:12: error: unsupported: call of a rejected header function 'Twice'
// CHECK-NEXT: normalize-header-reject.cpp:53:12: error: unsupported: call to library function 'Id'
// Then the header's own residuals, located in the header.
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:7:5: error: unsupported: static local variable
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:8:5: error: unsupported: assignment to something other than a local variable
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:12:12: error: unsupported: header function 'Store16' is overloaded (normalize header-inline: the subset has one function per name)
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:16:12: error: unsupported: header function 'Store16' is overloaded (normalize header-inline: the subset has one function per name)
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:20:12: error: unsupported: header function 'Scale' has the same name as a function of the main file (normalize header-inline: the subset has one function per name)
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:25:5: error: unsupported: variable of type 'int[2]'
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:26:12: error: unsupported: array subscript
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:30:12: error: unsupported: call of a rejected header function 'Inner'
// CHECK-NEXT: {{.*}}Inputs{{/|\\}}normalize-header-reject.h:33:15: error: unsupported: constexpr function
// CHECK-NOT: error

// OFF: normalize-header-reject.cpp:33:12: error: unsupported: call to library function 'Counter'
// OFF: normalize-header-reject.cpp:45:12: error: unsupported: call to library function 'Outer'
// OFF: normalize-header-reject.cpp:57:12: error: unsupported: call to library function 'Plus1'
