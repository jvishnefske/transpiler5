// FR-251: `s[i]` on a std::string is READ-ONLY in the subset (<=>
// `s.as_bytes()[i] as i8`); a write through it -- plain or compound -- is
// the nearest out-of-subset neighbour and a LOCATED residual: Rust has no
// safe byte write into a String (it could break UTF-8), and emitting one
// would need `unsafe`, which the rubric scores. The read on the line before
// stays in the subset (no diagnostic). Every residual is reported, exit 1,
// no output file.
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}2{{$}}
#include <cstdio>
#include <string>

int main() {
    std::string s = "abc";
    char first = s[0];
    // CHECK: string-byte-write.cpp:[[@LINE+1]]:5: error: unsupported: assignment to a std::string element (Rust has no safe String byte write)
    s[1] = 'x';
    // CHECK-NEXT: string-byte-write.cpp:[[@LINE+1]]:5: error: unsupported: assignment to a std::string element (Rust has no safe String byte write)
    s[2] += 1;
    std::printf("%s %d\n", s.c_str(), first);
    return 0;
}
