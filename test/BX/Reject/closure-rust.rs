// FR-252: Rust closures and callable parameters outside the canonical
// subset are LOCATED errors, each reported (parse errors stop at the first,
// so each is its own `split` unit below). The canonical closure is `let [mut]
// f = [move] |a: T, ..| [-> R] { .. };` -- every parameter typed, a block
// body, `let mut` EXACTLY when it modifies state or is passed as `&mut impl
// FnMut` (rustc would only warn about an extra `mut`, but the bijection needs
// one spelling); a callable parameter is `&impl Fn(..)` or `&mut impl
// FnMut(..)` (never `&impl FnMut`, `&mut impl Fn`, FnOnce), at most one per
// function (C++'s one template parameter F, which no local may shadow), never
// on a member function (a member template). The checker also refuses a
// closure that captures `self`, one that modifies its capture of an immutable
// binding (C++: the copy of a const is const), a nested closure, a closure
// argument without `&mut` / `&`, a closure used as a value, a void closure's
// call used as a value, a modifying closure called twice in one statement
// without a sequence point (C++ leaves the order unspecified; `f(f(1))` is
// E0499 besides), a `&impl Fn` parameter passed on as `&mut impl FnMut`, and
// a function with a callable parameter that no call with a closure reaches
// (its C++ template would never be instantiated, and only instantiations type
// it).
// RUN: split-file %s %t
// RUN: not %emitrust-bx %t/untyped.rs -o %t/untyped.out 2>&1 | FileCheck --check-prefix=UNTYPED %s
// RUN: not %emitrust-bx %t/exprbody.rs -o %t/exprbody.out 2>&1 | FileCheck --check-prefix=EXPRBODY %s
// RUN: not %emitrust-bx %t/fnmut-shared.rs -o %t/fnmut-shared.out 2>&1 | FileCheck --check-prefix=FNMUT-SHARED %s
// RUN: not %emitrust-bx %t/fn-mut.rs -o %t/fn-mut.out 2>&1 | FileCheck --check-prefix=FN-MUT %s
// RUN: not %emitrust-bx %t/fnonce.rs -o %t/fnonce.out 2>&1 | FileCheck --check-prefix=FNONCE %s
// RUN: not %emitrust-bx %t/wf.rs -o %t/wf.out 2> %t/wf.err
// RUN: FileCheck --check-prefix=WF %s --input-file=%t/wf.err
// RUN: grep -c "error:" %t/wf.err | FileCheck --check-prefix=WF-COUNT %s
// RUN: not test -e %t/wf.out
// UNTYPED: untyped.rs:2:15: error: unsupported: closure parameter without a type annotation
// EXPRBODY: exprbody.rs:2:22: error: unsupported: closure body that is not a block
// FNMUT-SHARED: fnmut-shared.rs:1:15: error: unsupported: `&impl FnMut` (the subset spells a callable parameter `&impl Fn(..)` or `&mut impl FnMut(..)`)
// FN-MUT: fn-mut.rs:1:19: error: unsupported: `&mut impl Fn` (the subset spells a callable parameter `&impl Fn(..)` or `&mut impl FnMut(..)`)
// FNONCE: fnonce.rs:1:15: error: unsupported: `&impl FnOnce` (the subset spells a callable parameter `&impl Fn(..)` or `&mut impl FnMut(..)`)
// WF:      wf.rs:8:9: error: unsupported: closure 'f' captures `this` / `self`
// WF-NEXT: wf.rs:14:9: error: unsupported: member function 'S::each' with a callable parameter (a member function template)
// WF-NEXT: wf.rs:19:1: error: unsupported: function 'two' with a callable parameter is never called with a closure (its C++ template would never be instantiated, and is typed only by its instantiations)
// WF-NEXT: wf.rs:19:1: error: unsupported: function 'two' with more than one callable parameter (the subset's template has the one type parameter F)
// WF-NEXT: wf.rs:24:1: error: unsupported: function 'named' with a callable parameter is never called with a closure (its C++ template would never be instantiated, and is typed only by its instantiations)
// WF-NEXT: wf.rs:24:1: error: unsupported: function 'named' with a callable parameter uses the name 'F' (its template parameter)
// WF-NEXT: wf.rs:29:1: error: unsupported: function 'pass_on' with a callable parameter is never called with a closure (its C++ template would never be instantiated, and is typed only by its instantiations)
// WF-NEXT: wf.rs:30:18: error: unsupported: callable parameter 'f' passed on as a mutable callable parameter
// WF-NEXT: wf.rs:43:5: error: unsupported: closure 'bump' is not spelled the canonical way (`let mut bump = ..`: `let mut` exactly when it modifies state or is passed as `&mut impl FnMut`)
// WF-NEXT: wf.rs:47:5: error: unsupported: closure 'set' modifies its capture of immutable 'fixed'
// WF-NEXT: wf.rs:48:9: error: assignment to immutable 'fixed'
// WF-NEXT: wf.rs:51:9: error: unsupported: closure 'inner' defined inside closure 'outer' (nested closures are not in the subset)
// WF-NEXT: wf.rs:55:24: error: unsupported: argument 1 of 'drive' binds a reference; the subset spells it `&mut x`
// WF-NEXT: wf.rs:56:18: error: unsupported: closure 'add' used other than by calling it or passing it to a callable parameter (copying a closure copies its state in C++ and moves it in Rust)
// WF-NEXT: wf.rs:57:18: error: call to void closure 'bump' used as a value
// WF-NEXT: wf.rs:63:24: error: unsupported: closure 'twice', which modifies state, is called twice in one statement without a sequence point between the calls (C++ leaves the order unspecified)
// WF-NEXT: wf.rs:64:5: error: unsupported: closure 'dbl' is not spelled the canonical way (`let dbl = ..`: `let mut` exactly when it modifies state or is passed as `&mut impl FnMut`)
// WF-COUNT: {{^}}17{{$}}
//--- untyped.rs
fn main() {
    let f = |a| -> i32 {
        return a;
    };
}
//--- exprbody.rs
fn main() {
    let f = |a: i32| a + 1;
}
//--- fnmut-shared.rs
fn g(f: &impl FnMut(i32)) {
}

fn main() {
}
//--- fn-mut.rs
fn g(f: &mut impl Fn(i32)) {
}

fn main() {
}
//--- fnonce.rs
fn g(f: &impl FnOnce(i32)) {
}

fn main() {
}
//--- wf.rs
#[derive(Clone)]
struct S {
    pub x: i32,
}

impl S {
    pub fn get(&self) -> i32 {
        let f = || -> i32 {
            return self.x;
        };
        return f();
    }

    pub fn each(&self, f: &impl Fn(i32)) {
        f(self.x);
    }
}

fn two(f: &impl Fn(i32), g: &impl Fn(i32)) {
    f(1);
    g(2);
}

fn named(f: &impl Fn(i32)) {
    let F: i32 = 1;
    f(F);
}

fn pass_on(f: &impl Fn(i32) -> i32) -> i32 {
    return drive(f, 1);
}

fn drive(f: &mut impl FnMut(i32) -> i32, x: i32) -> i32 {
    return f(x);
}

fn main() {
    let k: i32 = 3;
    let mut add = move |a: i32| -> i32 {
        return a + k;
    };
    let mut acc: i32 = 0;
    let bump = |a: i32| {
        acc += a;
    };
    let fixed: i32 = 1;
    let mut set = move || {
        fixed = 2;
    };
    let outer = || {
        let inner = || {
            return;
        };
    };
    let r: i32 = drive(add, 1);
    let q: i32 = add;
    let u: i32 = bump(2);
    let mut v: i32 = 0;
    let mut twice = move |x: i32| -> i32 {
        v += x;
        return v;
    };
    let w: i32 = twice(twice(1));
    let mut dbl = |a: i32| -> i32 {
        return a * 2;
    };
    print!("{} {} {} {} {}\n", r, q, u, w, dbl(4));
}
