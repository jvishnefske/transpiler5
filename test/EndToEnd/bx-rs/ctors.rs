// REQUIRES: cargo
// FR-257: emitrust-bx Rust -> C++, constructors end to end (rustc vs clang++
// byte-diff). `pub fn new(..) -> S { return S { .. }; }` (every field, in
// declaration order) <=> `S(..) : f(..), g(..) {}`; `S::new(a, b)` <=>
// `S(a, b)` as a let value, an argument, a vector element and a struct field
// value; a moved String parameter <=> `std::move`, a cloned one <=> a copy.
// A struct with a constructor is only built through it (a literal elsewhere
// would be aggregate initialization, which C++ forbids once a constructor is
// declared). Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
#[derive(Clone)]
struct Money {
    pub cents: i64,
    pub code: String,
}

impl Money {
    pub fn new(units: i64, mut code: String) -> Money {
        return Money { cents: units * 100i64, code };
    }

    pub fn add(&mut self, o: &Money) {
        self.cents += o.cents;
    }
}

#[derive(Clone)]
struct Order {
    pub id: u32,
    pub total: Money,
    pub lines: Vec<Money>,
}

impl Order {
    pub fn new(id: u32, mut first: Money) -> Order {
        return Order { id: id.wrapping_mul(7u32), total: first.clone(), lines: vec![first] };
    }

    pub fn line(&mut self, mut m: Money) {
        self.total.add(&m);
        self.lines.push(m);
    }
}

fn main() {
    let mut eur: String = String::from("EUR");
    let mut o: Order = Order::new(5u32, Money::new(3i64, eur.clone()));
    o.line(Money::new(-1i64, eur.clone()));
    o.line(Money::new(12i64, String::from("USD")));
    let p: Order = Order::new(6u32, Money::new(40i64, eur));
    print!("{} {} {} {}\n", o.id, o.total.cents, o.lines.len(), o.lines[2usize].code);
    print!("{} {} {}\n", p.id, p.total.cents, p.lines[0usize].code);
}
