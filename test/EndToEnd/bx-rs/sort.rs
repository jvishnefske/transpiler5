// REQUIRES: cargo
// FR-251: emitrust-bx Rust -> C++, a small algorithm over both containers
// (rustc vs clang++ byte-diff): insertion sort of a Vec<String> (String
// `<` <=> std::string `<`, which compares as unsigned bytes -- identical
// under the ASCII invariant) and of a Vec<i64> through `&mut Vec<T>`,
// element swaps through a temporary (`let t: String = w[j].clone();`
// copies; `w[j] = t;` moves), binary search over the sorted ints, dedup
// into a fresh vector with moves, and a frequency table built with
// `vec![0; n]`. Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn sort_words(w: &mut Vec<String>) {
    for i in 1usize..w.len() {
        let mut j: usize = i;
        while j > 0usize && w[j] < w[j.wrapping_sub(1usize)] {
            let mut t: String = w[j].clone();
            w[j] = w[j.wrapping_sub(1usize)].clone();
            w[j.wrapping_sub(1usize)] = t;
            j = j.wrapping_sub(1usize);
        }
    }
}

fn sort_ints(v: &mut Vec<i64>) {
    for i in 1usize..v.len() {
        let key: i64 = v[i];
        let mut j: usize = i;
        while j > 0usize && v[j.wrapping_sub(1usize)] > key {
            v[j] = v[j.wrapping_sub(1usize)];
            j = j.wrapping_sub(1usize);
        }
        v[j] = key;
    }
}

fn find(v: &Vec<i64>, x: i64) -> i32 {
    let mut lo: usize = 0usize;
    let mut hi: usize = v.len();
    while lo < hi {
        let mid: usize = lo.wrapping_add(hi.wrapping_sub(lo) / 2usize);
        if v[mid] < x {
            lo = mid.wrapping_add(1usize);
        } else {
            hi = mid;
        }
    }
    if lo < v.len() && v[lo] == x {
        return lo as i32;
    }
    return -1;
}

fn dedup(w: &Vec<String>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for s in w.iter() {
        if out.is_empty() || out[out.len().wrapping_sub(1usize)] != *s {
            let mut c: String = s.clone();
            c.push_str("");
            out.push(c);
        }
    }
    return out;
}

fn main() {
    let mut w: Vec<String> = vec![String::from("pear"), String::from("apple"), String::from("fig"),
                                  String::from("apple"), String::from("Zebra"), String::from("fig"),
                                  String::from("app")];
    sort_words(&mut w);
    for s in w.iter() {
        print!("{} ", s);
    }
    print!("\n");
    let d: Vec<String> = dedup(&w);
    print!("{} unique, first {} last {}\n", d.len(), d[0usize], d[d.len().wrapping_sub(1usize)]);
    let mut v: Vec<i64> = vec![42i64, -7i64, 19i64, 0i64, 1000000000000i64, -7i64, 3i64];
    sort_ints(&mut v);
    for x in v.iter() {
        print!("{} ", *x);
    }
    print!("\n");
    print!("{} {} {} {}\n", find(&v, 19i64), find(&v, -7i64), find(&v, 5i64), find(&v, 1000000000000i64));
    let mut freq: Vec<i32> = vec![0; 5usize];
    for x in v.iter() {
        let k: i64 = *x;
        if k >= 0i64 {
            freq[(k % 5i64) as usize] += 1;
        }
    }
    for i in 0usize..freq.len() {
        print!("{}:{} ", i, freq[i]);
    }
    print!("\n");
}
