// FR-254 R3 under --normalize, C++ -> canonical subset C++ and Rust. A
// POSTFIX increment/decrement used as a value that the subset refuses (on
// `*p` or a pointer local; a local integer `i++` is already a Kernel IncDec)
// is rewritten to "read the old value in place, then a separate `++x;`", and
// `return E;` to `const T bx_ret = E; ++x; return bx_ret;`. Pinned: the
// four measured woff2/brunsli shapes (an index through `(*offset)++` in a
// range-for, a call on the right-hand side, a pointer post-increment store,
// a `return` of a post-increment read), a declaration with a post-DECREMENT
// inside an R1-rewritten stride loop (rules compose), the fresh temporary
// skipping a name the translation unit declares (`bx_ret` -> `bx_ret1`), the
// located `note: normalized:` per applied rule, laws 1-2 on the OUTPUT
// (--check-roundtrip re-imports the printed C++ WITHOUT --normalize), the
// cpp -> cpp fixed point, and the OFF run: without --normalize every site
// keeps its pre-R3 refusal at the `++`.
// RUN: %emitrust-bx --normalize --check-roundtrip %s 2> %t.notes | FileCheck %s
// RUN: FileCheck --check-prefix=NOTE --input-file=%t.notes %s
// RUN: %emitrust-bx --normalize --emit=cpp %s -o %t.cpp 2> /dev/null
// RUN: FileCheck --check-prefix=CPP --input-file=%t.cpp %s
// RUN: %emitrust-bx --emit=cpp %t.cpp | diff %t.cpp -
// RUN: not %emitrust-bx %s 2>&1 | FileCheck --check-prefix=OFF %s
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

int ValueMarker(int tag) {
    return tag * 8 + 2;
}

void Pack(std::uint8_t* dst, std::size_t* offset, const std::vector<int>& v) {
    for (int x : v) {
        const std::uint8_t packed_byte = static_cast<std::uint8_t>(x & 0xff);
        dst[(*offset)++] = packed_byte;
    }
}

void Marks(int* data, std::size_t* pos, int tag) {
    data[(*pos)++] = ValueMarker(tag);
}

void StoreBase128(std::uint8_t* data, int val, int len) {
    for (int i = 0; i < len; ++i) {
        *(data++) = (val & 0x7f) | (i + 1 < len ? 0x80 : 0);
        val >>= 7;
    }
}

int ReadByte(const std::uint8_t* data, std::size_t* pos) {
    return data[(*pos)++];
}

int Strided(const int* d, std::size_t* pos, int n) {
    int bx_ret = 0;
    for (int i = 0; i < n; i += 3) {
        const int b = d[(*pos)--];
        bx_ret += b * i;
    }
    return bx_ret + d[(*pos)++];
}

int main() {
    std::vector<std::uint8_t> buf(8, 0);
    std::vector<int> v = {300, 7, 41};
    std::size_t off = 1;
    Pack(buf.data(), &off, v);
    StoreBase128(buf.data() + 4, 1000, 2);
    std::vector<int> ints(8, 5);
    std::size_t pos = 2;
    Marks(ints.data(), &pos, 3);
    std::size_t at = 3;
    const int b = ReadByte(buf.data(), &at);
    std::size_t back = 6;
    const int s = Strided(ints.data(), &back, 7);
    std::printf("%d %d %d %d %d %d %d\n", static_cast<int>(off), buf[2], buf[5], ints[2], b, s,
                static_cast<int>(back));
    return 0;
}

// NOTE: normalize-incdec.cpp:33:13: note: normalized: post-incdec
// NOTE-NEXT: normalize-incdec.cpp:38:10: note: normalized: post-incdec
// NOTE-NEXT: normalize-incdec.cpp:43:11: note: normalized: post-incdec
// NOTE-NEXT: normalize-incdec.cpp:49:17: note: normalized: post-incdec
// NOTE-NEXT: normalize-incdec.cpp:54:5: note: normalized: for->while
// NOTE-NEXT: normalize-incdec.cpp:55:25: note: normalized: post-incdec
// NOTE-NEXT: normalize-incdec.cpp:58:23: note: normalized: post-incdec

// OFF: normalize-incdec.cpp:33:13: error: unsupported: increment/decrement used as a value
// OFF: normalize-incdec.cpp:38:10: error: unsupported: increment/decrement used as a value
// OFF: normalize-incdec.cpp:43:11: error: unsupported: increment/decrement used as a value
// OFF: normalize-incdec.cpp:49:17: error: unsupported: increment/decrement used as a value
// OFF: normalize-incdec.cpp:54:5: error: unsupported: `for` loop outside the canonical counting form
// OFF: normalize-incdec.cpp:58:23: error: unsupported: increment/decrement used as a value

// CHECK: fn ValueMarker(mut tag: i32) -> i32 {
// CHECK-NEXT:     return (tag * 8) + 2;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn Pack(mut dst: &mut [u8], mut offset: &mut [usize], v: &Vec<i32>) {
// CHECK-NEXT:     for mut x in v.iter().copied() {
// CHECK-NEXT:         let packed_byte: u8 = (x & 255i32) as u8;
// CHECK-NEXT:         dst[offset[0usize]] = packed_byte;
// CHECK-NEXT:         offset[0usize] = offset[0usize].wrapping_add(1usize);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn Marks(mut data: &mut [i32], mut pos: &mut [usize], mut tag: i32) {
// CHECK-NEXT:     data[pos[0usize]] = ValueMarker(tag);
// CHECK-NEXT:     pos[0usize] = pos[0usize].wrapping_add(1usize);
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn StoreBase128(mut data: &mut [u8], mut val: i32, mut len: i32) {
// CHECK-NEXT:     for i in 0..len {
// CHECK-NEXT:         data[0usize] = ((val & 127i32) | (if (i + 1) < len { 128i32 } else { 0i32 })) as u8;
// CHECK-NEXT:         data = &mut data[1usize..];
// CHECK-NEXT:         val >>= 7;
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn ReadByte(mut data: &[u8], mut pos: &mut [usize]) -> i32 {
// CHECK-NEXT:     let bx_ret1: i32 = data[pos[0usize]] as i32;
// CHECK-NEXT:     pos[0usize] = pos[0usize].wrapping_add(1usize);
// CHECK-NEXT:     return bx_ret1;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn Strided(mut d: &[i32], mut pos: &mut [usize], mut n: i32) -> i32 {
// CHECK-NEXT:     let mut bx_ret: i32 = 0;
// CHECK-NEXT:     {
// CHECK-NEXT:         let mut i: i32 = 0;
// CHECK-NEXT:         while i < n {
// CHECK-NEXT:             let b: i32 = d[pos[0usize]];
// CHECK-NEXT:             pos[0usize] = pos[0usize].wrapping_sub(1usize);
// CHECK-NEXT:             bx_ret += b * i;
// CHECK-NEXT:             i += 3;
// CHECK-NEXT:         }
// CHECK-NEXT:     }
// CHECK-NEXT:     let bx_ret2: i32 = bx_ret + d[pos[0usize]];
// CHECK-NEXT:     pos[0usize] = pos[0usize].wrapping_add(1usize);
// CHECK-NEXT:     return bx_ret2;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut buf: Vec<u8> = vec![0u8; 8usize];
// CHECK-NEXT:     let mut v: Vec<i32> = vec![300, 7, 41];
// CHECK-NEXT:     let mut off: usize = 1usize;
// CHECK-NEXT:     Pack(&mut buf[..], std::slice::from_mut(&mut off), &v);
// CHECK-NEXT:     StoreBase128(&mut buf[4usize..], 1000, 2);
// CHECK-NEXT:     let mut ints: Vec<i32> = vec![5; 8usize];
// CHECK-NEXT:     let mut pos: usize = 2usize;
// CHECK-NEXT:     Marks(&mut ints[..], std::slice::from_mut(&mut pos), 3);
// CHECK-NEXT:     let mut at: usize = 3usize;
// CHECK-NEXT:     let b: i32 = ReadByte(&buf[..], std::slice::from_mut(&mut at));
// CHECK-NEXT:     let mut back: usize = 6usize;
// CHECK-NEXT:     let s: i32 = Strided(&ints[..], std::slice::from_mut(&mut back), 7);
// CHECK-NEXT:     print!("{} {} {} {} {} {} {}\n", off as i32, buf[2usize] as i32, buf[5usize] as i32, ints[2usize], b, s, back as i32);
// CHECK-NEXT: }

// CPP: #include <cstdio>
// CPP-NEXT: #include <vector>
// CPP-EMPTY:
// CPP-NEXT: int ValueMarker(int tag) {
// CPP-NEXT:     return (tag * 8) + 2;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Pack(unsigned char* dst, std::size_t* offset, const std::vector<int>& v) {
// CPP-NEXT:     for (int x : v) {
// CPP-NEXT:         const unsigned char packed_byte = static_cast<unsigned char>(x & 255);
// CPP-NEXT:         dst[*offset] = packed_byte;
// CPP-NEXT:         ++*offset;
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void Marks(int* data, std::size_t* pos, int tag) {
// CPP-NEXT:     data[*pos] = ValueMarker(tag);
// CPP-NEXT:     ++*pos;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: void StoreBase128(unsigned char* data, int val, int len) {
// CPP-NEXT:     for (int i = 0; i < len; ++i) {
// CPP-NEXT:         *data = static_cast<unsigned char>((val & 127) | ((i + 1) < len ? 128 : 0));
// CPP-NEXT:         ++data;
// CPP-NEXT:         val >>= 7;
// CPP-NEXT:     }
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int ReadByte(const unsigned char* data, std::size_t* pos) {
// CPP-NEXT:     const int bx_ret1 = static_cast<int>(data[*pos]);
// CPP-NEXT:     ++*pos;
// CPP-NEXT:     return bx_ret1;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int Strided(const int* d, std::size_t* pos, int n) {
// CPP-NEXT:     int bx_ret = 0;
// CPP-NEXT:     {
// CPP-NEXT:         int i = 0;
// CPP-NEXT:         while (i < n) {
// CPP-NEXT:             const int b = d[*pos];
// CPP-NEXT:             --*pos;
// CPP-NEXT:             bx_ret += b * i;
// CPP-NEXT:             i += 3;
// CPP-NEXT:         }
// CPP-NEXT:     }
// CPP-NEXT:     const int bx_ret2 = bx_ret + d[*pos];
// CPP-NEXT:     ++*pos;
// CPP-NEXT:     return bx_ret2;
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<unsigned char> buf(static_cast<std::size_t>(8), static_cast<unsigned char>(0));
// CPP-NEXT:     std::vector<int> v = {300, 7, 41};
// CPP-NEXT:     std::size_t off = static_cast<std::size_t>(1);
// CPP-NEXT:     Pack(buf.data(), &off, v);
// CPP-NEXT:     StoreBase128(buf.data() + 4, 1000, 2);
// CPP-NEXT:     std::vector<int> ints(static_cast<std::size_t>(8), 5);
// CPP-NEXT:     std::size_t pos = static_cast<std::size_t>(2);
// CPP-NEXT:     Marks(ints.data(), &pos, 3);
// CPP-NEXT:     std::size_t at = static_cast<std::size_t>(3);
// CPP-NEXT:     const int b = ReadByte(buf.data(), &at);
// CPP-NEXT:     std::size_t back = static_cast<std::size_t>(6);
// CPP-NEXT:     const int s = Strided(ints.data(), &back, 7);
// CPP-NEXT:     std::printf("%d %d %d %d %d %d %d\n", static_cast<int>(off), static_cast<int>(buf[static_cast<std::size_t>(2)]), static_cast<int>(buf[static_cast<std::size_t>(5)]), ints[static_cast<std::size_t>(2)], b, s, static_cast<int>(back));
// CPP-NEXT:     return 0;
// CPP-NEXT: }
