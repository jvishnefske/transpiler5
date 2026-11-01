// REQUIRES: cargo
// FR-253: emitrust-bx Rust -> C++, the algorithm rows without a callable
// (rustc vs clang++ byte-diff): `v.sort_unstable()` <=> std::sort, `v.sort()`
// <=> std::stable_sort, `v.reverse()`, `v.fill(x)` (a String place is cloned:
// C++ copies), `v.contains(&x)` <=> `std::find(..) != v.end()` (`!` <=> `==
// v.end()`), `bx::count(&v, &x)` <=> std::count (i64), `v.iter().fold(init,
// |a, &e| a + e)` <=> std::accumulate in the INIT's type (`e as i64` widens,
// `wrapping_add` for unsigned), `*v.iter().max().unwrap()` /
// `*v.iter().min().unwrap()` <=> `*std::max_element` / `*std::min_element`,
// `std::cmp::max/min`. The containers are reached through `&Vec` / `&mut
// Vec` parameters and a struct field as well as locals. Inputs from an LCG.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
#[derive(Clone)]
struct Bag {
    pub items: Vec<i64>,
    pub tag: i32,
}

fn make(mut n: i32, mut seed: u32) -> Vec<i32> {
    let mut v: Vec<i32> = Vec::new();
    let mut x: u32 = seed;
    for i in 0..n {
        x = x.wrapping_mul(1103515245u32).wrapping_add(12345u32);
        v.push(((x >> 16u32) % 41u32) as i32 - 20);
    }
    return v;
}

fn tidy(v: &mut Vec<i32>, mut flip: bool) -> i32 {
    v.sort_unstable();
    if flip {
        v.reverse();
    }
    return v[0usize] * 1000 + v[v.len().wrapping_sub(1usize)];
}

fn stats(v: &Vec<i32>, mut probe: i32) -> i64 {
    let mut hit: bool = v.contains(&probe);
    let mut miss: bool = !v.contains(&(probe + 100));
    let mut c: i64 = bx::count(v, &probe);
    let mut s: i32 = v.iter().fold(0, |a, &e| a + e);
    let mut w: i64 = v.iter().fold(3000000000i64, |a, &e| a + e as i64);
    let mut hi: i32 = *v.iter().max().unwrap();
    let mut lo: i32 = *v.iter().min().unwrap();
    print!("stats {}: {} {} {} {} {} {} {}\n", probe, hit as i32, miss as i32, c, s, w, hi, lo);
    return c + w;
}

fn main() {
    let mut v: Vec<i32> = make(21, 7u32);
    let mut ends: i32 = tidy(&mut v, false);
    let mut w: Vec<i32> = make(9, 4u32);
    w.sort();
    let mut ends2: i32 = tidy(&mut w, true);
    print!("tidy: {} {} {} {}\n", ends, ends2, v[10usize], w[4usize]);
    let mut total: i64 = 0i64;
    for k in -3..4 {
        total += stats(&v, v[(k * 3i32 + 10i32) as usize] + k % 2);
    }
    let mut u: Vec<u32> = vec![4000000000u32, 400000000u32, 5u32];
    let mut uw: u32 = u.iter().fold(1u32, |a, &e| a.wrapping_add(e));
    let mut bytes: Vec<u8> = vec![250u8, 7u8, 128u8];
    let mut bs: u64 = bytes.iter().fold(0u64, |a, &e| a.wrapping_add(e as u64));
    let mut d: Vec<f64> = vec![0.1, 0.2, -0.0];
    let mut ds: f64 = d.iter().fold(-0.0, |a, &e| a + e);
    let mut ints_as_f: f64 = v.iter().fold(0.5, |a, &e| a + e as f64);
    print!("folds: {} {} {} {} {}\n", total, uw, bs, bx::fmt_f(ds, 3), bx::fmt_f(ints_as_f, 1));
    let mut bag: Bag = Bag { items: vec![9i64, -2i64, 9i64, 4i64], tag: 3 };
    bag.items.fill(bag.tag as i64);
    let mut nine: i64 = bx::count(&bag.items, &9i64);
    bag.items[1usize] = 8i64;
    bag.items.sort_unstable();
    print!("bag: {} {} {} {}\n", nine, bag.items[0usize], bag.items[3usize], bx::count(&bag.items, &3i64));
    let mut names: Vec<String> = vec![String::from("kiwi"), String::from("fig"), String::from("date")];
    let mut who: String = String::from("yam");
    names.sort();
    let mut first: String = names[0usize].clone();
    names.fill(who.clone());
    print!("names: {} {} {}\n", first, names[2usize], names.contains(&first) as i32);
    let mut m: i32 = std::cmp::max(ends, ends2) - std::cmp::min(v[0usize], -7);
    let mut z: usize = std::cmp::min(v.len(), 5usize);
    print!("minmax: {} {}\n", m, z);
}

mod bx {
    pub fn fmt_f(x: f64, prec: usize) -> String {
        if x.is_nan() {
            if x.is_sign_negative() {
                return String::from("-nan");
            }
            return String::from("nan");
        }
        format!("{:.*}", prec, x)
    }

    pub fn count<T: PartialEq>(v: &[T], x: &T) -> i64 {
        let mut n: i64 = 0;
        for e in v.iter() {
            if *e == *x {
                n += 1;
            }
        }
        n
    }
}
