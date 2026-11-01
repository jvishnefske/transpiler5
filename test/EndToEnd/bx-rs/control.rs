// REQUIRES: cargo
// FR-250: emitrust-bx Rust -> C++, control flow end to end (rustc vs
// clang++ byte-diff). Pins: if / else if / else, `while` with break and
// continue, nested half-open `for` ranges (i32 and usize), early `return;`
// from a unit fn, recursion and mutual recursion in DEFINITION order that
// C++ needs prototypes for (fn `ping` calls `pong` defined later), and
// `fn main()` <=> `int main() { ... return 0; }`. Distinct values at every
// call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
fn ping(n: i32, acc: i64) -> i64 {
    if n <= 0 {
        return acc;
    }
    return pong(n - 1, acc * 3i64 + (n as i64));
}

fn pong(n: i32, acc: i64) -> i64 {
    if n <= 0 {
        return acc;
    }
    return ping(n - 2, acc - (n as i64));
}

fn ackermann(m: i64, n: i64) -> i64 {
    if m == 0i64 {
        return n + 1i64;
    } else if n == 0i64 {
        return ackermann(m - 1i64, 1i64);
    } else {
        return ackermann(m - 1i64, ackermann(m, n - 1i64));
    }
}

fn scan(limit: i32, skip: i32) {
    if limit < 0 {
        print!("negative\n");
        return;
    }
    let mut i: i32 = 0;
    let mut hits: i32 = 0;
    while i < limit {
        i += 1;
        if i % skip == 0 {
            continue;
        }
        if i * i > 300 {
            break;
        }
        hits += i;
    }
    print!("scan {} {} {} {}\n", limit, skip, i, hits);
}

fn main() {
    print!("{} {} {}\n", ping(5, 1i64), ping(8, 2i64), pong(9, -4i64));
    print!("{} {} {}\n", ackermann(2i64, 3i64), ackermann(3i64, 3i64), ackermann(1i64, 9i64));
    scan(10, 3);
    scan(50, 4);
    scan(-1, 2);
    let mut grid: usize = 0usize;
    for r in 0usize..4usize {
        for c in r..6usize {
            if r.wrapping_add(c) % 2usize == 0usize {
                grid = grid.wrapping_add(r.wrapping_mul(10usize)).wrapping_add(c);
            }
        }
    }
    print!("grid {}\n", grid);
}
