// FR-251: the String ASCII invariant. Every String in the subset is ASCII,
// so byte indexing (`s.as_bytes()[i] as i8`) and substr never split a UTF-8
// character and a C++ char is exactly a Rust byte. A non-ASCII literal --
// initializer, `+=`, a char literal, a std::string temporary -- is the
// nearest out-of-subset neighbour and a LOCATED residual (bytes pushed at
// run time go through bx::push_byte, which panics LOUDLY instead).
// RUN: not %emitrust-bx %s -o %t.rs 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.rs
// COUNT: {{^}}4{{$}}
#include <cstdio>
#include <string>

int main() {
    std::string ok = "plain";
    // CHECK: non-ascii.cpp:[[@LINE+1]]:21: error: unsupported: non-ASCII string literal (every String is ASCII so byte indexing never splits a character)
    std::string s = "caf\xc3\xa9";
    // CHECK-NEXT: non-ascii.cpp:[[@LINE+1]]:10: error: unsupported: non-ASCII string literal (every String is ASCII so byte indexing never splits a character)
    s += "\xe2\x82\xac";
    // CHECK-NEXT: non-ascii.cpp:[[@LINE+1]]:11: error: unsupported: non-ASCII character literal
    ok += '\xe9';
    // CHECK-NEXT: non-ascii.cpp:[[@LINE+1]]:33: error: unsupported: non-ASCII string literal (every String is ASCII so byte indexing never splits a character)
    std::string t = std::string("na\xefve") + ok;
    std::printf("%s %s %s\n", ok.c_str(), s.c_str(), t.c_str());
    return 0;
}
