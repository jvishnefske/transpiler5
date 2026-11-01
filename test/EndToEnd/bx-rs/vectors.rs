// REQUIRES: cargo
// FR-251: emitrust-bx Rust -> C++, the Vec rows end to end (rustc vs clang++
// byte-diff). Pins `Vec::new()` / `vec![..]` / `vec![x; n]` (and a typed
// `Vec::<T>::new()` where rustc could not infer T) <=> `std::vector<T> v;` /
// `{..}` / `(n, x)` / `std::vector<T>{}`, push/pop/clear/resize, len /
// is_empty, indexing with read, write and compound write, `==`/`!=` on
// vectors, nested vectors, a sieve over Vec<u8>, and a stack machine that
// pops only when non-empty (pop on an empty vector is UB in C++, so the
// subset's tests never do it). Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn primes_below(n: usize) -> Vec<usize> {
    let mut sieve: Vec<u8> = vec![1u8; n];
    let mut out: Vec<usize> = Vec::new();
    for i in 2usize..n {
        if (sieve[i] as i32) == 1 {
            out.push(i);
            let mut j: usize = i.wrapping_mul(i);
            while j < n {
                sieve[j] = 0u8;
                j = j.wrapping_add(i);
            }
        }
    }
    return out;
}

fn eval(prog: &Vec<i64>) -> i64 {
    let mut stack: Vec<i64> = Vec::new();
    for op in prog.iter().copied() {
        if op >= 0i64 {
            stack.push(op);
        } else if stack.len() >= 2usize {
            let b: i64 = stack[stack.len().wrapping_sub(1usize)];
            stack.pop();
            let a: i64 = stack[stack.len().wrapping_sub(1usize)];
            stack.pop();
            if op == -1i64 {
                stack.push(a + b);
            } else if op == -2i64 {
                stack.push(a * b);
            } else {
                stack.push(a - b);
            }
        }
    }
    if stack.is_empty() {
        return -999i64;
    }
    return stack[stack.len().wrapping_sub(1usize)];
}

fn show(v: &Vec<i32>) {
    print!("[");
    for i in 0usize..v.len() {
        if i > 0usize {
            print!(" ");
        }
        print!("{}", v[i]);
    }
    print!("] n={} empty={}\n", v.len(), v.is_empty() as i32);
}

fn main() {
    let ps: Vec<usize> = primes_below(60usize);
    print!("{} primes, last {}\n", ps.len(), ps[ps.len().wrapping_sub(1usize)]);
    let p1: Vec<i64> = vec![3i64, 4i64, -1i64, 5i64, -2i64];
    let p2: Vec<i64> = vec![10i64, 2i64, 7i64, -3i64, -2i64, -1i64];
    let p3: Vec<i64> = vec![-1i64];
    print!("{} {} {}\n", eval(&p1), eval(&p2), eval(&p3));
    let mut v: Vec<i32> = vec![4, -9, 16];
    show(&v);
    v.push(25);
    v[1usize] = v[1usize] * -3;
    v[0usize] += 100;
    v[2usize] -= 1;
    show(&v);
    v.resize(7usize, -6);
    show(&v);
    v.resize(3usize, 0);
    v.pop();
    show(&v);
    let w: Vec<i32> = vec![104, 27];
    print!("{} {}\n", (v == w) as i32, (v != w) as i32);
    v.clear();
    show(&v);
    print!("{}\n", Vec::<i32>::new().len());
    let mut grid: Vec<Vec<i32>> = vec![vec![0; 3usize]; 2usize];
    grid[1usize][2usize] = 8;
    grid[0usize].push(5);
    grid.push(vec![1, 2]);
    for r in 0usize..grid.len() {
        show(&grid[r]);
    }
}
