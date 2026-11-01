// FR-253: Rust spellings next to the algorithm rows are LOCATED errors (parse
// errors stop at the first, so each is its own `split` unit; the checker's
// are collected in wf.rs). Parse level: a fold closure other than the one
// canonical `|a, &e| a + e` (any other operation is a closure variable,
// `.copied().fold(init, &f)`), iterator adaptors outside the rows (`.sum()`,
// `.any` on `.iter()` without `.copied()`, `.max().copied()`), an extreme
// not dereferenced at once, `.map(&f)` outside `w.extend(..)`, `.extend()`
// of anything but a mapped copied iterator, and `sort_by` (a closure literal
// argument: the comparator row is `bx::sort_by_less(&mut v, &cmp)` with a
// closure VARIABLE). Checker level: the f64 rows (sort_unstable, `*max`,
// std::cmp::max -- refused like their C++ twins), a closure argument's `&` /
// `&mut` (exactly when it modifies state), a borrowed argument's `&x` /
// `&mut x`, the fold's canonical body for its accumulator and element types,
// an accumulator that cannot hold the elements, `v.fill(v[0])` and
// `v.extend(v.iter()..)` (second borrows), a closure of the wrong signature,
// and a mutating row on an immutable binding.
// RUN: split-file %s %t
// RUN: not %emitrust-bx %t/fold.rs -o %t/fold.out 2>&1 | FileCheck --check-prefix=FOLD %s
// RUN: not %emitrust-bx %t/sum.rs -o %t/sum.out 2>&1 | FileCheck --check-prefix=SUM %s
// RUN: not %emitrust-bx %t/any-ref.rs -o %t/any-ref.out 2>&1 | FileCheck --check-prefix=ANYREF %s
// RUN: not %emitrust-bx %t/unwrap.rs -o %t/unwrap.out 2>&1 | FileCheck --check-prefix=UNWRAP %s
// RUN: not %emitrust-bx %t/max-copied.rs -o %t/max-copied.out 2>&1 | FileCheck --check-prefix=MAXCOPIED %s
// RUN: not %emitrust-bx %t/map.rs -o %t/map.out 2>&1 | FileCheck --check-prefix=MAP %s
// RUN: not %emitrust-bx %t/extend.rs -o %t/extend.out 2>&1 | FileCheck --check-prefix=EXTEND %s
// RUN: not %emitrust-bx %t/sort-by.rs -o %t/sort-by.out 2>&1 | FileCheck --check-prefix=SORTBY %s
// RUN: not %emitrust-bx %t/wf.rs -o %t/wf.out 2> %t/wf.err
// RUN: FileCheck --check-prefix=WF %s --input-file=%t/wf.err
// RUN: grep -c "error:" %t/wf.err | FileCheck --check-prefix=WF-COUNT %s
// RUN: not test -e %t/wf.out
// FOLD: fold.rs:2:29: error: unsupported: fold closure other than `|a, &e| a + e` (the canonical std::accumulate step: `a.wrapping_add(e)` for an unsigned accumulator, `e as T` when the element type differs; any other operation is a closure variable `.copied().fold(init, &f)`)
// SUM: sum.rs:2:30: error: unsupported: method call `.sum()`
// ANYREF: any-ref.rs:5:21: error: unsupported: iterator method `.any()` (the subset's iterator chains are `v.iter().fold(init, |a, &e| ..)`, `v.iter().max().unwrap()` / `.min()`, and `v.iter().copied()` followed by `.fold(init, &f)`, `.any(&p)`, `.all(&p)`, `.for_each(&f)` or, inside `w.extend(..)`, `.map(&f)`)
// UNWRAP: unwrap.rs:2:18: error: unsupported: `v.iter().max().unwrap()` other than dereferenced at once (`*v.iter().max().unwrap()`)
// MAXCOPIED: max-copied.rs:2:21: error: unsupported: `.max()` other than `*v.iter().max().unwrap()`
// MAP: map.rs:6:5: error: unsupported: expression statement that is not a call
// EXTEND: extend.rs:2:16: error: unsupported: `.extend()` of something other than `v.iter().copied().map(&f)`
// SORTBY: sort-by.rs:2:15: error: unsupported: closure
// WF:      wf.rs:2:5: error: unsupported: std::sort of a vector of double without a comparator (-0.0 and 0.0 are equivalent but print differently, so the result depends on the algorithm; f64 has no total order in Rust)
// WF-NEXT: wf.rs:11:39: error: unsupported: closure argument 'q' of std::count_if is not spelled the canonical way (`&mut q`: `&mut` exactly when it modifies state)
// WF-NEXT: wf.rs:12:39: error: unsupported: closure argument 'p' of std::count_if is not spelled the canonical way (`&p`: `&mut` exactly when it modifies state)
// WF-NEXT: wf.rs:13:45: error: unsupported: closure argument 'p' of std::any_of is not spelled the canonical way (`&p`: `&mut` exactly when it modifies state)
// WF-NEXT: wf.rs:14:32: error: unsupported: an argument of std::count is not spelled the canonical way (`&x`)
// WF-NEXT: wf.rs:15:34: error: unsupported: an argument of std::find is not spelled the canonical way (`&x`)
// WF-NEXT: wf.rs:16:22: error: unsupported: the fold of std::accumulate is not spelled the canonical way (`|a, &e| a + e` for a i32 accumulator over i32 elements)
// WF-NEXT: wf.rs:17:23: error: unsupported: the fold of std::accumulate is not spelled the canonical way (`|a, &e| a + e as i64` for a i64 accumulator over i32 elements)
// WF-NEXT: wf.rs:18:39: error: unsupported: std::accumulate of i64 elements from an initial value of type i32 (C++ sums in the initial value's type, converting every step; normalizer: give the initial value a type that holds every element, e.g. `0LL` / `0i64`)
// WF-NEXT: wf.rs:19:23: error: unsupported: std::max_element over a vector of double (it returns the FIRST extreme, and -0.0 / 0.0 compare equal but print differently; f64 has no total order in Rust)
// WF-NEXT: wf.rs:20:23: error: unsupported: std::max of two doubles (it returns its first argument unless the second compares greater, which differs from f64::max on NaN and signed zero)
// WF-NEXT: wf.rs:21:12: error: unsupported: the value of std::fill reads 'v', the vector it fills (a second borrow in Rust)
// WF-NEXT: wf.rs:22:39: error: unsupported: closure 'p' has type impl Fn(i32) -> bool, but std::transform calls it as impl Fn(i32) -> i64 (the subset has no implicit conversion of an argument or result)
// WF-NEXT: wf.rs:23:5: error: unsupported: std::transform appends to 'v' while reading it (iterator invalidation in C++, a second borrow in Rust)
// WF-NEXT: wf.rs:27:19: error: unsupported: an argument of std::transform is not spelled the canonical way (`&mut x`)
// WF-NEXT: wf.rs:33:5: error: std::stable_sort of immutable 'v'
// WF-COUNT: {{^}}16{{$}}
//--- fold.rs
fn f(v: &Vec<i32>) -> i32 {
    return v.iter().fold(0, |acc, &e| acc + e);
}

fn main() {
}
//--- sum.rs
fn f(v: &Vec<i32>) -> i32 {
    return v.iter().copied().sum();
}

fn main() {
}
//--- any-ref.rs
fn f(v: &Vec<i32>) -> bool {
    let p = |a: i32| -> bool {
        return a > 0;
    };
    return v.iter().any(&p);
}

fn main() {
}
//--- unwrap.rs
fn f(v: &Vec<i32>) -> i32 {
    let m: i32 = v.iter().max().unwrap();
    return m;
}

fn main() {
}
//--- max-copied.rs
fn f(v: &Vec<i32>) -> i32 {
    return v.iter().max().copied().unwrap();
}

fn main() {
}
//--- map.rs
fn f(v: &Vec<i32>) {
    let g = |a: i32| -> i32 {
        return a;
    };
    let mut w: Vec<i32> = Vec::new();
    v.iter().copied().map(&g);
}

fn main() {
}
//--- extend.rs
fn f(v: &mut Vec<i32>, w: &Vec<i32>) {
    v.extend(w.iter().copied());
}

fn main() {
}
//--- sort-by.rs
fn f(v: &mut Vec<i32>) {
    v.sort_by(|a, b| a.cmp(b));
}

fn main() {
}
//--- wf.rs
fn g(mut v: Vec<i32>, mut d: Vec<f64>, mut big: Vec<i64>) -> i64 {
    d.sort_unstable();
    let p = |a: i32| -> bool {
        return a > 0;
    };
    let mut cnt: i32 = 0;
    let mut q = |a: i32| -> bool {
        cnt += 1;
        return a > 1;
    };
    let mut n: i64 = bx::count_if(&v, &q);
    let mut m: i64 = bx::count_if(&v, p);
    let mut k: bool = v.iter().copied().any(&mut p);
    let mut c: i64 = bx::count(v, &3);
    let mut h: bool = v.contains(3);
    let mut t: i32 = v.iter().fold(0, |a, &e| a.wrapping_add(e));
    let mut t2: i64 = v.iter().fold(0i64, |a, &e| a + e);
    let mut t3: i32 = big.iter().fold(0, |a, &e| a + e as i32);
    let mut hi: f64 = *d.iter().max().unwrap();
    let mut mx: f64 = std::cmp::max(d[0usize], 1.0);
    v.fill(v[0usize]);
    big.extend(v.iter().copied().map(&p));
    v.extend(v.iter().copied().map(&p));
    let id = |a: i32| -> i32 {
        return a;
    };
    bx::transform(v, &id);
    return n + m + c;
}

fn main() {
    let v: Vec<i32> = vec![3, 1];
    v.sort();
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
}
