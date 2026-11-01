// REQUIRES: cargo
// FR-257: emitrust-bx Rust -> C++, `impl` blocks end to end (rustc vs
// clang++ byte-diff). `pub fn m(&self)` <=> a const member function,
// `&mut self` <=> non-const, an associated fn `S::make(..)` <=> a static
// member function (`crate::geo::Vec2::zero()` <=> `::geo::Vec2::zero()`);
// `self.f` <=> `this->f`, `self.g()` <=> `this->g()`; a method taking `&S`
// of its own type; the two-phase-borrow receiver shapes the subset accepts
// (`h.record(h.len2())` reads the receiver in an argument of a `&mut self`
// call; `self.record(self.count)` inside a method). The C++ side defines
// every member function out of line after the class. Distinct values at
// every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
mod geo {
    #[derive(Clone)]
    pub struct Vec2 {
        pub x: i64,
        pub y: i64,
    }

    impl Vec2 {
        pub fn zero() -> Vec2 {
            return Vec2 { x: 0i64, y: 0i64 };
        }

        pub fn dot(&self, o: &Vec2) -> i64 {
            return self.x * o.x + self.y * o.y;
        }

        pub fn add(&mut self, o: &Vec2) {
            self.x += o.x;
            self.y += o.y;
        }

        pub fn scaled(&self, k: i64) -> Vec2 {
            return Vec2 { x: self.x * k, y: self.y * k };
        }
    }
}

#[derive(Clone)]
struct History {
    pub items: Vec<i32>,
    pub count: i32,
}

impl History {
    pub fn record(&mut self, v: i32) {
        self.items.push(v);
        self.count += 1;
    }

    pub fn len2(&self) -> i32 {
        return (self.items.len() as i32) * 2;
    }

    pub fn echo(&mut self) {
        self.record(self.count);
        self.record(self.len2() + 100);
    }

    pub fn last(&self) -> i32 {
        return self.items[self.items.len().wrapping_sub(1usize)];
    }
}

fn main() {
    let mut a: crate::geo::Vec2 = crate::geo::Vec2::zero();
    let b: crate::geo::Vec2 = crate::geo::Vec2 { x: 3i64, y: -4i64 };
    a.add(&b);
    a.add(&b.scaled(5i64));
    let c: crate::geo::Vec2 = a.scaled(-2i64);
    print!("{} {} {} {}\n", a.x, a.y, a.dot(&b), c.dot(&a));
    let mut h: History = History { items: Vec::new(), count: 0 };
    h.record(7);
    h.record(h.len2());
    h.echo();
    h.record(h.last() * 3);
    print!("{} {} {} {}\n", h.count, h.len2(), h.last(), h.items[2usize]);
    let mut hs: Vec<History> = vec![h.clone(), History { items: vec![1, 2], count: 2 }];
    hs[1usize].echo();
    for x in hs.iter_mut() {
        x.record(9);
    }
    print!("{} {} {}\n", hs[0usize].last(), hs[1usize].count, h.count);
}
