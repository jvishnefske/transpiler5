// FR-257: the Rust side of the struct subset is DEFINED by rs_parse + the
// checker, and every well-formedness violation is reported (located, all of
// them). Pins: a struct literal of a struct that has `new` (C++ would call the
// constructor), a move out of a field (C++ cannot partially move), the
// receiver-alias shapes rustc rejects -- `c.take(&c)` (E0502),
// `c.set(bump(&mut c))` (E0499), `cells[0].set(cells[0].get())` (E0502,
// IndexMut is not two-phase) -- a `&mut self` call on an immutable binding,
// an associated fn called on an object and a method called without one, and a
// counting loop whose bound `c.v.len()` the body changes (C++ re-reads the
// bound every iteration, Rust evaluates the range once). The checker says so
// in its own located terms instead of emitting C++.
// RUN: not %emitrust-bx %s -o %t.cpp 2> %t.err
// RUN: FileCheck %s --input-file=%t.err
// RUN: grep -c "error:" %t.err | FileCheck --check-prefix=COUNT %s
// RUN: not test -e %t.cpp
// COUNT: {{^}}10{{$}}
#[derive(Clone)]
struct Cell {
    pub v: Vec<i32>,
    pub n: i32,
    pub name: String,
}

impl Cell {
    pub fn get(&self) -> i32 {
        return self.n;
    }

    pub fn set(&mut self, k: i32) {
        self.n = k;
    }

    pub fn take(&mut self, o: &Cell) {
        self.n += o.n;
    }

    pub fn zero() -> i32 {
        return 0;
    }
}

#[derive(Clone)]
struct Pt {
    pub x: i32,
}

impl Pt {
    pub fn new(x: i32) -> Pt {
        return Pt { x };
    }
}

fn bump(c: &mut Cell) -> i32 {
    c.n += 1;
    return c.n;
}

fn main() {
    let mut c: Cell = Cell { v: vec![1], n: 2, name: String::from("c") };
    // CHECK: struct-rust.rs:[[@LINE+1]]:17: error: unsupported: aggregate initialization of 'Pt', which has a constructor (C++ would call the constructor)
    let p: Pt = Pt { x: 4 };
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:21: error: unsupported: move out of a data member (the subset copies it: C++ copy, Rust `.clone()`)
    let s: String = c.name;
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:7: error: unsupported: 'c' is passed by reference to 'take', a non-const member function called on it (aliasing)
    c.take(&c);
    // CHECK-NEXT: struct-rust.rs:[[@LINE+2]]:7: error: unsupported: an argument of 'set' modifies 'c', the object it is called on (a second borrow in Rust)
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:11: error: unsupported: 'c' is passed to 'bump' by non-const reference and also used elsewhere in the statement (C++ leaves the order unspecified)
    c.set(bump(&mut c));
    let mut cells: Vec<Cell> = vec![c.clone()];
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:19: error: unsupported: non-const member function 'set' is called on an element of 'cells' with an argument that reads 'cells' (a second borrow in Rust)
    cells[0usize].set(cells[0usize].get());
    let q: Cell = c.clone();
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:5: error: call of non-const member function 'set' of immutable 'q'
    q.set(3);
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:20: error: static member function 'Cell::zero' called on an object
    let z: i32 = c.zero();
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:18: error: member function 'Cell::get' called without an object
    let k: i32 = Cell::get();
    // CHECK-NEXT: struct-rust.rs:[[@LINE+1]]:5: error: unsupported: counting loop body modifies or shadows 'c', whose size is the bound
    for i in 0usize..c.v.len() {
        c.v.push(1);
    }
    print!("{} {} {} {}\n", p.x, s, z, k);
}
