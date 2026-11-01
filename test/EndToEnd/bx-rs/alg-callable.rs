// REQUIRES: cargo
// FR-253: emitrust-bx Rust -> C++, the algorithm rows that take a closure
// (rustc vs clang++ byte-diff): `bx::count_if(&v, &p)`, `v.iter().copied()
// .any(&p)` / `.all(&p)` (`!..any` <=> std::none_of), `.copied().fold(init,
// &f)` <=> std::accumulate with an operation, `.copied().for_each(&mut f)`,
// `bx::transform(&mut v, &f)` <=> in-place std::transform, `w.extend(v.iter()
// .copied().map(&f))` <=> std::transform into std::back_inserter(w), and
// `bx::sort_by_less(&mut v, &cmp)` <=> `std::sort(b, e, cmp)` -- libstdc++'s
// introsort, so a comparator with distinguishable equivalents (|a| < |b|) and
// a call-counting comparator (`&mut`, FnMut) agree with clang++ to the last
// comparison. A `move` closure keeps the value it captured when its source
// changes later. Strict weak orders only. Inputs from an LCG.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn make(mut n: i32, mut seed: u32, mut range: u32) -> Vec<i32> {
    let mut v: Vec<i32> = Vec::new();
    let mut x: u32 = seed;
    for i in 0..n {
        x = x.wrapping_mul(1103515245u32).wrapping_add(12345u32);
        v.push(((x >> 16u32) % range) as i32 - (range / 2u32) as i32);
    }
    return v;
}

fn digest(v: &Vec<i32>) -> u64 {
    let mut h: u64 = 1469598103934665603u64;
    for x in v.iter().copied() {
        h = (h ^ x as u64).wrapping_mul(1099511628211u64);
    }
    return h;
}

fn predicates(mut seed: u32) -> i64 {
    let mut v: Vec<i32> = make(30, seed, 61u32);
    let mut lim: i32 = (seed % 9u32) as i32;
    let big = move |a: i32| -> bool {
        return a > lim;
    };
    lim = -1000;
    let mut asked: i32 = 0;
    let mut neg = |a: i32| -> bool {
        asked += 1;
        return a < 0;
    };
    let mut n: i64 = bx::count_if(&v, &big);
    let mut m: i64 = bx::count_if(&v, &mut neg);
    let mut any: bool = v.iter().copied().any(&mut neg);
    let mut all: bool = v.iter().copied().all(&mut neg);
    let mut none: bool = !v.iter().copied().any(&big);
    print!("pred {}: {} {} {} {} {} {} {}\n", seed, n, m, any as i32, all as i32, none as i32, asked, lim);
    return n * 100i64 + m;
}

fn folds(mut seed: u32) {
    let mut v: Vec<i32> = make(11, seed, 21u32);
    let horner = |acc: i64, d: i32| -> i64 {
        return acc * 5i64 + d as i64;
    };
    let mut h: i64 = v.iter().copied().fold(2i64, &horner);
    let mut total: i64 = 0i64;
    let mut each = |a: i32| {
        total = total * 3i64 + a as i64;
    };
    v.iter().copied().for_each(&mut each);
    v.iter().copied().for_each(&mut each);
    print!("fold {}: {} {}\n", seed, h, total);
}

fn transforms(mut seed: u32) {
    let mut v: Vec<i32> = make(9, seed, 101u32);
    let mut k: i32 = (seed % 4u32) as i32;
    let shift = move |a: i32| -> i32 {
        return a * 3 - k;
    };
    k = 77;
    bx::transform(&mut v, &shift);
    let mut out: Vec<i64> = vec![-1i64];
    let mut seq: i64 = 0i64;
    let mut tag = |a: i32| -> i64 {
        seq += 1i64;
        return a as i64 * 1000i64 + seq;
    };
    out.extend(v.iter().copied().map(&mut tag));
    print!("transform {}: {} {} {} {} {}\n", seed, v[0usize], v[8usize], out[1usize], out[9usize], k);
}

fn sorts(mut seed: u32) {
    let mut v: Vec<i32> = make(200, seed, 11u32);
    let by_abs = |a: i32, b: i32| -> bool {
        let mut x: i32 = if a < 0 { -a } else { a };
        let mut y: i32 = if b < 0 { -b } else { b };
        return x < y;
    };
    bx::sort_by_less(&mut v, &by_abs);
    print!("abs {}: {}", seed, digest(&v));
    for i in 0..20 {
        print!(" {}", v[(i * 10i32) as usize]);
    }
    print!("\n");
    let mut calls: i64 = 0i64;
    let mut trace: u64 = 0u64;
    let mut desc = |a: i32, b: i32| -> bool {
        calls += 1i64;
        trace = trace.wrapping_mul(1000003u64).wrapping_add((a * 131i32 + b) as u64);
        return a > b;
    };
    let mut w: Vec<i32> = make(150, seed.wrapping_add(1u32), 1000u32);
    bx::sort_by_less(&mut w, &mut desc);
    let mut small: Vec<i32> = make(12, seed, 9u32);
    bx::sort_by_less(&mut small, &mut desc);
    let mut empty: Vec<i32> = Vec::new();
    bx::sort_by_less(&mut empty, &mut desc);
    print!("desc {}: {} {} {} {} {}\n", seed, calls, trace, w[0usize], w[149usize], digest(&small));
}

fn main() {
    let mut t: i64 = predicates(5u32);
    t += predicates(2026u32);
    print!("total {}\n", t);
    folds(3u32);
    folds(4040u32);
    transforms(17u32);
    transforms(90210u32);
    sorts(1u32);
    sorts(31337u32);
}

mod bx {
    pub fn count_if<T: Copy>(v: &[T], mut p: impl FnMut(T) -> bool) -> i64 {
        let mut n: i64 = 0;
        for e in v.iter() {
            if p(*e) {
                n += 1;
            }
        }
        n
    }

    pub fn transform<T: Copy>(v: &mut [T], mut f: impl FnMut(T) -> T) {
        for e in v.iter_mut() {
            *e = f(*e);
        }
    }

    pub fn sort_by_less<T: Copy>(v: &mut [T], mut less: impl FnMut(T, T) -> bool) {
        let n: usize = v.len();
        if n == 0 {
            return;
        }
        let depth: usize = 2 * (usize::BITS - 1 - n.leading_zeros()) as usize;
        sbl_loop(v, 0, n, depth, &mut less);
        if n > 16 {
            sbl_insertion(v, 0, 16, &mut less);
            for i in 16..n {
                sbl_linear_insert(v, i, &mut less);
            }
        } else {
            sbl_insertion(v, 0, n, &mut less);
        }
    }

    fn sbl_loop<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], first: usize, mut last: usize, mut depth: usize,
                                                less: &mut F) {
        while last - first > 16 {
            if depth == 0 {
                sbl_heap_sort(v, first, last, less);
                return;
            }
            depth -= 1;
            let mid: usize = first + (last - first) / 2;
            sbl_median_to_first(v, first, first + 1, mid, last - 1, less);
            let cut: usize = sbl_partition(v, first + 1, last, first, less);
            sbl_loop(v, cut, last, depth, less);
            last = cut;
        }
    }

    fn sbl_median_to_first<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], r: usize, a: usize, b: usize,
                                                           c: usize, less: &mut F) {
        if less(v[a], v[b]) {
            if less(v[b], v[c]) {
                v.swap(r, b);
            } else if less(v[a], v[c]) {
                v.swap(r, c);
            } else {
                v.swap(r, a);
            }
        } else if less(v[a], v[c]) {
            v.swap(r, a);
        } else if less(v[b], v[c]) {
            v.swap(r, c);
        } else {
            v.swap(r, b);
        }
    }

    fn sbl_partition<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], mut first: usize, mut last: usize,
                                                     pivot: usize, less: &mut F) -> usize {
        loop {
            while less(v[first], v[pivot]) {
                first += 1;
            }
            last -= 1;
            while less(v[pivot], v[last]) {
                last -= 1;
            }
            if first >= last {
                return first;
            }
            v.swap(first, last);
            first += 1;
        }
    }

    fn sbl_linear_insert<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], mut last: usize, less: &mut F) {
        let val: T = v[last];
        let mut next: usize = last - 1;
        while less(val, v[next]) {
            v[last] = v[next];
            last = next;
            next -= 1;
        }
        v[last] = val;
    }

    fn sbl_insertion<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], first: usize, last: usize, less: &mut F) {
        if first == last {
            return;
        }
        for i in first + 1..last {
            if less(v[i], v[first]) {
                v[first..=i].rotate_right(1);
            } else {
                sbl_linear_insert(v, i, less);
            }
        }
    }

    fn sbl_adjust_heap<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], base: usize, mut hole: usize, len: usize,
                                                       value: T, less: &mut F) {
        let top: usize = hole;
        let mut second: usize = hole;
        while len >= 1 && second < (len - 1) / 2 {
            second = 2 * (second + 1);
            if less(v[base + second], v[base + second - 1]) {
                second -= 1;
            }
            v[base + hole] = v[base + second];
            hole = second;
        }
        if len & 1 == 0 && len >= 2 && second == (len - 2) / 2 {
            second = 2 * (second + 1);
            v[base + hole] = v[base + second - 1];
            hole = second - 1;
        }
        let mut parent: usize = hole.wrapping_sub(1) / 2;
        while hole > top && less(v[base + parent], value) {
            v[base + hole] = v[base + parent];
            hole = parent;
            parent = hole.wrapping_sub(1) / 2;
        }
        v[base + hole] = value;
    }

    fn sbl_heap_sort<T: Copy, F: FnMut(T, T) -> bool>(v: &mut [T], first: usize, last: usize, less: &mut F) {
        let len: usize = last - first;
        if len >= 2 {
            let mut parent: usize = (len - 2) / 2;
            loop {
                let value: T = v[first + parent];
                sbl_adjust_heap(v, first, parent, len, value, less);
                if parent == 0 {
                    break;
                }
                parent -= 1;
            }
        }
        let mut l: usize = last;
        while l - first > 1 {
            l -= 1;
            let value: T = v[l];
            v[l] = v[first];
            sbl_adjust_heap(v, first, 0, l - first, value, less);
        }
    }
}
