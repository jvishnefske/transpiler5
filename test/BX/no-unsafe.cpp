// FR-250: zero `unsafe` in anything emitrust-bx emits, INCLUDING the bx
// helper module (both helpers are forced in here via %f and %c). The rubric
// scores unsafe; a helper that relocated it would not be a solution.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=HELPERS --input-file=%t.crate/src/main.rs %s
// COUNT: {{^}}0{{$}}
// HELPERS: mod bx {
// HELPERS: pub fn fmt_f(x: f64, prec: usize) -> String {
// HELPERS: pub fn chr(x: i32) -> char {
#include <cstdio>

int main() {
    std::printf("%c %.3f\n", 'z', 2.5);
    return 0;
}
