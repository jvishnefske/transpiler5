// FR-253: Rust -> C++, the algorithm rows read from the Rust side, law 2 at
// the CLI (the C++ printed for this file prints byte-identical C++ when fed
// back in; its Rust is this file's canonical Rust, byte for byte) and laws 1-2 under
// --check-roundtrip. Pins: inside a module the helpers are `crate::bx::..`
// (bare `bx::` would resolve relative to the module), `std::cmp::min` stays
// absolute, `&mut over` (a closure that modifies state) <=> passing `over`,
// `bx::count_if(v, ..)` / `bx::transform(v, ..)` / `bx::sort_by_less(v, ..)`
// on a `&mut Vec` parameter (bare: it already is a reference), an owned
// vector's `w.extend(v.iter().copied().map(&widen))` <=> std::back_inserter,
// `w.iter().fold(0u64, |a, &e| a.wrapping_add(e))` <=> `std::accumulate(..,
// 0ULL)`, and the `&&` of a contains and a `!contains` (each parenthesized in
// C++: `std::find(..) != v.end()` is an equality expression). The canonical
// `mod bx` holds exactly the four FR-253 helpers this file uses.
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: %emitrust-bx --emit=cpp %t.cpp -o %t.2.cpp
// RUN: diff %t.cpp %t.2.cpp
// RUN: %emitrust-bx --emit=rust %t.cpp -o %t.rs
// RUN: %emitrust-bx --emit=rust %s -o %t.self.rs
// RUN: diff %t.rs %t.self.rs
// RUN: %emitrust-bx --check-roundtrip --emit=rust %s | FileCheck %s
// RUN: FileCheck --check-prefix=CPP %s --input-file=%t.cpp
// CHECK:      mod stats {
// CHECK-NEXT:     pub fn summary(v: &mut Vec<i32>, mut k: i32) -> i64 {
// CHECK-NEXT:         let mut hits: i32 = 0;
// CHECK-NEXT:         let mut over = |a: i32| -> bool {
// CHECK-NEXT:             hits += 1;
// CHECK-NEXT:             return a > k;
// CHECK-NEXT:         };
// CHECK-NEXT:         let twice = |a: i32| -> i32 {
// CHECK-NEXT:             return a * 2;
// CHECK-NEXT:         };
// CHECK-NEXT:         let less = |a: i32, b: i32| -> bool {
// CHECK-NEXT:             return (a % 10) < (b % 10);
// CHECK-NEXT:         };
// CHECK-NEXT:         let mut n: i64 = crate::bx::count_if(v, &mut over);
// CHECK-NEXT:         let mut c: i64 = crate::bx::count(v, &k);
// CHECK-NEXT:         crate::bx::transform(v, &twice);
// CHECK-NEXT:         crate::bx::sort_by_less(v, &less);
// CHECK-NEXT:         let mut lo: i32 = std::cmp::min(*v.iter().min().unwrap(), k);
// CHECK-NEXT:         k = 0;
// CHECK-NEXT:         return (n * 1000i64) + c + (lo as i64) + (hits as i64) + (k as i64);
// CHECK-NEXT:     }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: fn main() {
// CHECK-NEXT:     let mut v: Vec<i32> = vec![14, 3, 27, 8];
// CHECK-NEXT:     let mut w: Vec<u64> = Vec::new();
// CHECK-NEXT:     let widen = |a: i32| -> u64 {
// CHECK-NEXT:         return a as u64;
// CHECK-NEXT:     };
// CHECK-NEXT:     w.extend(v.iter().copied().map(&widen));
// CHECK-NEXT:     w.sort_unstable();
// CHECK-NEXT:     w.reverse();
// CHECK-NEXT:     let mut total: u64 = w.iter().fold(0u64, |a, &e| a.wrapping_add(e));
// CHECK-NEXT:     let mut r: i64 = crate::stats::summary(&mut v, 5);
// CHECK-NEXT:     let mut seen: bool = v.contains(&16) && !w.contains(&3u64);
// CHECK-NEXT:     print!("{} {} {} {} {}\n", r, total, seen as i32, w[0usize], v[0usize]);
// CHECK-NEXT: }
// CPP:      namespace stats {
// CPP-EMPTY:
// CPP-NEXT: long long summary(std::vector<int>& v, int k) {
// CPP-NEXT:     int hits = 0;
// CPP-NEXT:     auto over = [&hits, &k](const int a) -> bool {
// CPP-NEXT:         ++hits;
// CPP-NEXT:         return a > k;
// CPP-NEXT:     };
// CPP-NEXT:     auto twice = [](const int a) -> int {
// CPP-NEXT:         return a * 2;
// CPP-NEXT:     };
// CPP-NEXT:     auto less = [](const int a, const int b) -> bool {
// CPP-NEXT:         return (a % 10) < (b % 10);
// CPP-NEXT:     };
// CPP-NEXT:     long long n = std::count_if(v.begin(), v.end(), over);
// CPP-NEXT:     long long c = std::count(v.begin(), v.end(), k);
// CPP-NEXT:     std::transform(v.begin(), v.end(), v.begin(), twice);
// CPP-NEXT:     std::sort(v.begin(), v.end(), less);
// CPP-NEXT:     int lo = std::min(*std::min_element(v.begin(), v.end()), k);
// CPP-NEXT:     k = 0;
// CPP-NEXT:     return (n * 1000LL) + c + static_cast<long long>(lo) + static_cast<long long>(hits) + static_cast<long long>(k);
// CPP-NEXT: }
// CPP-EMPTY:
// CPP-NEXT: }  // namespace stats
// CPP-EMPTY:
// CPP-NEXT: int main() {
// CPP-NEXT:     std::vector<int> v = {14, 3, 27, 8};
// CPP-NEXT:     std::vector<unsigned long long> w;
// CPP-NEXT:     auto widen = [](const int a) -> unsigned long long {
// CPP-NEXT:         return static_cast<unsigned long long>(a);
// CPP-NEXT:     };
// CPP-NEXT:     std::transform(v.begin(), v.end(), std::back_inserter(w), widen);
// CPP-NEXT:     std::sort(w.begin(), w.end());
// CPP-NEXT:     std::reverse(w.begin(), w.end());
// CPP-NEXT:     unsigned long long total = std::accumulate(w.begin(), w.end(), 0ULL);
// CPP-NEXT:     long long r = ::stats::summary(v, 5);
// CPP-NEXT:     bool seen = (std::find(v.begin(), v.end(), 16) != v.end()) && (std::find(w.begin(), w.end(), 3ULL) == w.end());
// CPP-NEXT:     std::printf("%lld %llu %d %llu %d\n", r, total, static_cast<int>(seen), w[static_cast<std::size_t>(0)], v[static_cast<std::size_t>(0)]);
// CPP-NEXT:     return 0;
// CPP-NEXT: }
mod stats {
    pub fn summary(v: &mut Vec<i32>, mut k: i32) -> i64 {
        let mut hits: i32 = 0;
        let mut over = |a: i32| -> bool {
            hits += 1;
            return a > k;
        };
        let twice = |a: i32| -> i32 {
            return a * 2;
        };
        let less = |a: i32, b: i32| -> bool {
            return (a % 10) < (b % 10);
        };
        let mut n: i64 = crate::bx::count_if(v, &mut over);
        let mut c: i64 = crate::bx::count(v, &k);
        crate::bx::transform(v, &twice);
        crate::bx::sort_by_less(v, &less);
        let mut lo: i32 = std::cmp::min(*v.iter().min().unwrap(), k);
        k = 0;
        return (n * 1000i64) + c + (lo as i64) + (hits as i64) + (k as i64);
    }
}

fn main() {
    let mut v: Vec<i32> = vec![14, 3, 27, 8];
    let mut w: Vec<u64> = Vec::new();
    let widen = |a: i32| -> u64 {
        return a as u64;
    };
    w.extend(v.iter().copied().map(&widen));
    w.sort_unstable();
    w.reverse();
    let mut total: u64 = w.iter().fold(0u64, |a, &e| a.wrapping_add(e));
    let mut r: i64 = crate::stats::summary(&mut v, 5);
    let mut seen: bool = v.contains(&16) && !w.contains(&3u64);
    print!("{} {} {} {} {}\n", r, total, seen as i32, w[0usize], v[0usize]);
}

mod bx {
    pub fn count<T: PartialEq>(v: &[T], x: &T) -> i64 {
        let mut n: i64 = 0;
        for e in v.iter() {
            if *e == *x {
                n += 1;
            }
        }
        n
    }

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
