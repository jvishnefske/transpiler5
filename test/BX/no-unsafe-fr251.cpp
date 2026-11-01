// FR-251: zero `unsafe` in anything emitrust-bx emits for the vector /
// string / reference rows, INCLUDING the three new bx helpers (push_byte,
// substr, substr_from -- all forced in here) and the reference parameters
// and range-for loops. The rubric scores unsafe; a helper that relocated it
// (an unchecked byte write, an unchecked slice) would not be a solution.
// RUN: %emitrust-bx --emit=crate %s -o %t.crate
// RUN: grep -c unsafe %t.crate/src/main.rs > %t.count || true
// RUN: FileCheck --check-prefix=COUNT --input-file=%t.count %s
// RUN: FileCheck --check-prefix=HELPERS --input-file=%t.crate/src/main.rs %s
// COUNT: {{^}}0{{$}}
// HELPERS: fn bump(v: &mut Vec<i32>, s: &String) {
// HELPERS: for x in v.iter_mut() {
// HELPERS: mod bx {
// HELPERS: pub fn push_byte(s: &mut String, c: i8) {
// HELPERS: pub fn substr(s: &str, pos: usize, n: usize) -> String {
// HELPERS: pub fn substr_from(s: &str, pos: usize) -> String {
#include <cstdio>
#include <string>
#include <vector>

void bump(std::vector<int>& v, const std::string& s) {
    for (int& x : v) {
        x += static_cast<int>(s.size());
    }
}

int main() {
    std::vector<int> v = {1, 2};
    std::string s = "abc";
    s.push_back('d');
    bump(v, s);
    std::printf("%s %s %d\n", s.substr(1, 2).c_str(), s.substr(2).c_str(), v[0]);
    return 0;
}
