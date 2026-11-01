// REQUIRES: cargo
// FR-257: emitrust-bx Rust -> C++, structs end to end (rustc vs clang++
// byte-diff). `#[derive(Clone)] struct` with `pub` fields <=> a C++
// aggregate; a literal `S { a: x, b }` (every field in declaration order,
// field-init shorthand accepted) <=> `S{x, b}` / `S s = {x, b};`; field reads,
// `p.f = e`, `p.f += e` through locals, elements, nested members and `&mut S`
// parameters; `.clone()` <=> a C++ copy, a bare move <=> `std::move`; a struct
// inside a module used from the root (`crate::inv::Item`) <=> `::inv::Item`;
// a Vec of structs grown, indexed and iterated (`.iter()`, `.iter_mut()`,
// `.iter().cloned()`). u32 arithmetic is `.wrapping_*` <=> C++ unsigned.
// Distinct values at every call site.
// RUN: rustc -O --edition 2021 %s -o %t.rsbin
// RUN: %emitrust-bx --emit=cpp %s -o %t.cpp
// RUN: clang++ -std=c++17 %t.cpp -o %t.native
// RUN: %t.rsbin > %t.rs.out
// RUN: %t.native > %t.native.out
// RUN: diff %t.rs.out %t.native.out
mod inv {
    #[derive(Clone)]
    pub struct Item {
        pub name: String,
        pub qty: u32,
        pub price: f64,
    }

    pub fn restock(it: &mut Item, n: u32) {
        it.qty = it.qty.wrapping_add(n);
        it.price = it.price * 0.5;
    }
}

#[derive(Clone)]
struct Shelf {
    pub label: String,
    pub items: Vec<crate::inv::Item>,
    pub slot: i32,
}

fn worth(s: &Shelf) -> f64 {
    let mut w: f64 = 0.0;
    for it in s.items.iter() {
        w += (it.qty as f64) * it.price;
    }
    return w;
}

fn relabel(mut s: Shelf, suffix: &String) -> Shelf {
    s.label.push_str(suffix);
    s.slot = s.slot * 10;
    return s;
}

fn main() {
    let nut: crate::inv::Item = crate::inv::Item { name: String::from("nut"), qty: 4u32, price: 2.5 };
    let mut label: String = String::from("A");
    let mut shelf: Shelf = Shelf { label, items: vec![nut.clone()], slot: 3 };
    shelf.items.push(crate::inv::Item { name: String::from("bolt"), qty: 0u32, price: 8.0 });
    crate::inv::restock(&mut shelf.items[1usize], 7u32);
    shelf.items[0usize].qty = shelf.items[0usize].qty.wrapping_sub(5u32);
    let mut copy: Shelf = shelf.clone();
    shelf.slot += 4;
    let mut moved: Shelf = relabel(copy, &String::from("-b"));
    print!("{} {} {} {}\n", shelf.label, shelf.slot, shelf.items.len(), bx::fmt_f(worth(&shelf), 3));
    print!("{} {} {} {}\n", moved.label, moved.slot, bx::fmt_f(worth(&moved), 3), nut.qty);
    let mut all: Vec<Shelf> = vec![shelf.clone(), moved];
    for s in all.iter_mut() {
        s.slot -= 1;
    }
    let mut names: String = String::new();
    for mut s in all.iter().cloned() {
        s.label.push_str("!");
        names.push_str(&s.label);
    }
    print!("{} {} {} {}\n", names, all[0usize].slot, all[1usize].slot, shelf.items[0usize].qty);
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
}
