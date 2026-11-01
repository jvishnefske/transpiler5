// REQUIRES: cargo
// FR-261: emitrust-bx Rust -> C++, byte slices end to end (rustc vs clang++
// byte-diff). `&[u8]` / `&mut [u8]` parameters <=> `const unsigned char*` /
// `unsigned char*` (a non-`mut` binding is `T* const`), a `&mut [usize]`
// cursor written through `pos[0usize]` <=> `*pos`, `&mut buf[..]` <=>
// `buf.data()`, `&data[2usize..]` <=> `data.data() + 2`, the empty suffix
// `&data[data.len()..]` <=> `data.data() + data.size()` (one past the end),
// `std::slice::from_mut(&mut pos)` <=> `&pos`. Lengths are separate integers:
// `crc(&data[..], 3usize)` reads a prefix of a longer slice. Distinct values
// at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn crc(data: &[u8], n: usize) -> u32 {
    let mut c: u32 = 4294967295u32;
    for i in 0usize..n {
        c = c ^ (data[i] as u32);
        for k in 0..8 {
            if (c & 1u32) != 0u32 {
                c = (c >> 1) ^ 3988292384u32;
            } else {
                c = c >> 1;
            }
        }
    }
    return !c;
}

fn put_u16(out: &mut [u8], pos: &mut [usize], v: u32) {
    out[pos[0usize]] = ((v >> 8u32) & 255u32) as u8;
    out[pos[0usize].wrapping_add(1usize)] = (v & 255u32) as u8;
    pos[0usize] = pos[0usize].wrapping_add(2usize);
}

fn get_u16(data: &[u8], pos: &mut [usize]) -> u32 {
    let v: u32 = ((data[pos[0usize]] as u32) << 8u32) | (data[pos[0usize].wrapping_add(1usize)] as u32);
    pos[0usize] = pos[0usize].wrapping_add(2usize);
    return v;
}

fn main() {
    let mut buf: Vec<u8> = vec![0u8; 12usize];
    let mut pos: usize = 1usize;
    put_u16(&mut buf[..], std::slice::from_mut(&mut pos), 4660u32);
    put_u16(&mut buf[..], std::slice::from_mut(&mut pos), 65535u32);
    put_u16(&mut buf[2usize..], std::slice::from_mut(&mut pos), 513u32);
    let mut rd: usize = 1usize;
    let a: u32 = get_u16(&buf[..], std::slice::from_mut(&mut rd));
    let b: u32 = get_u16(&buf[..], std::slice::from_mut(&mut rd));
    print!("{} {} {} {}\n", pos, rd, a, b);
    let data: Vec<u8> = vec![49u8, 50u8, 51u8, 52u8, 53u8, 54u8, 55u8, 56u8, 57u8];
    print!("{} {} {} {}\n", crc(&data[..], data.len()), crc(&data[..], 3usize), crc(&data[2usize..], 4usize),
           crc(&data[data.len()..], 0usize));
    for i in 0usize..buf.len() {
        print!("{} ", buf[i] as i32);
    }
    print!("\n");
}
