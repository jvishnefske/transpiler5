// REQUIRES: cargo
// FR-261: emitrust-bx Rust -> C++, one-element record slices end to end
// (rustc vs clang++ byte-diff). A `&mut [Reader]` / `&[Reader]` parameter is
// a `Reader*` / `const Reader*` (C++ `s->f`, `s->m(..)`): fields read and
// written through `r[0usize].f`, `&self` and `&mut self` methods called
// through `r[0usize].m(..)`, `std::slice::from_mut(self)` <=> `this` handed
// to a free function from a method, `std::slice::from_mut(&mut rd)` <=> `&rd`
// at the call site, `std::slice::from_mut(&mut rd.errors)` <=> `&rd.errors`,
// and a slice of several records `&mut regs[1usize..]` <=> `regs.data() + 1`
// walked by index. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
mod io {
    #[derive(Clone)]
    pub struct Reader {
        pub data: Vec<u8>,
        pub pos: usize,
        pub errors: i32,
    }

    impl Reader {
        pub fn left(&self) -> usize {
            return self.data.len().wrapping_sub(self.pos);
        }

        pub fn next(&mut self) -> i32 {
            if self.pos >= self.data.len() {
                bump(std::slice::from_mut(&mut self.errors), 1);
                return -1;
            }
            let b: i32 = self.data[self.pos] as i32;
            self.pos = self.pos.wrapping_add(1usize);
            return b;
        }

        pub fn skip_all(&mut self) -> i32 {
            return drain(std::slice::from_mut(self));
        }
    }

    pub fn bump(n: &mut [i32], by: i32) {
        n[0usize] += by;
    }

    pub fn drain(r: &mut [Reader]) -> i32 {
        let mut t: i32 = 0;
        while r[0usize].left() > 0usize {
            t = (t * 7) + r[0usize].next();
        }
        return t;
    }
}

fn total(rs: &mut [crate::io::Reader], n: usize) -> i32 {
    let mut t: i32 = 0;
    for i in 0usize..n {
        t += rs[i].next();
        rs[i].errors += 10;
    }
    return t;
}

fn main() {
    let mut rd: crate::io::Reader = crate::io::Reader { data: vec![9u8, 8u8, 7u8], pos: 1usize, errors: 0 };
    let first: i32 = crate::io::drain(std::slice::from_mut(&mut rd));
    let past: i32 = rd.next();
    crate::io::bump(std::slice::from_mut(&mut rd.errors), 5);
    print!("{} {} {} {}\n", first, past, rd.errors, rd.pos);
    let mut regs: Vec<crate::io::Reader> = vec![rd.clone(), crate::io::Reader { data: vec![1u8, 2u8], pos: 0usize, errors: 3 },
                                                crate::io::Reader { data: vec![4u8], pos: 0usize, errors: 4 }];
    let t: i32 = total(&mut regs[1usize..], 2usize);
    let s: i32 = regs[1usize].skip_all();
    print!("{} {} {} {} {}\n", t, s, regs[0usize].errors, regs[1usize].errors, regs[2usize].errors);
}
