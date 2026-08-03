// FR-61e slice 2: parameter names. A function carrying the discardable
// `emitrust.param_names` array binds each non-receiver argument under its
// slot's spelling through the same per-function uniquifier as named locals:
// an unused named parameter is `_`-prefixed (lint-legal), a body local
// spelled like a parameter uniquifies to `<name>_1` (the parameter seeds
// the set first, since arguments are named before body ops), a method
// ignores slot 0 and keeps the hard-wired `&mut self`, and a function
// without the attribute keeps v-numbering untouched. Empty slots (unnamed
// C parameters, by-value shadows whose variable already took the name)
// also keep vN.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The FR-61 target shape: the parameter name reaches every use, the
// binding folds into the tail if-expression (FR-61d slice 3), and the
// arm-local subtraction/addition inline while the call binding survives.
// CHECK-LABEL: fn sum_to(n: i32) -> i32 {
// CHECK-NEXT:    if n <= 0i32 {
// CHECK-NEXT:        0i32
// CHECK-NEXT:    } else {
// CHECK-NEXT:        let v5: i32 = sum_to(n - 1i32);
// CHECK-NEXT:        n + v5
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @sum_to(%arg0: i32) -> i32
    attributes {emitrust.param_names = ["n"]} {
  %zero = emitrust.constant <0 : i32> : i32
  %one = emitrust.constant <1 : i32> : i32
  %cond = emitrust.cmp le, %arg0, %zero : (i32, i32) -> i1
  %r = emitrust.let mut %zero : i32
  emitrust.if %cond {
    emitrust.assign %r = %zero : i32
  } else {
    %sub = emitrust.sub %arg0, %one : i32
    %call = emitrust.call_opaque "sum_to"(%sub) : (i32) -> i32
    %add = emitrust.add %arg0, %call : i32
    emitrust.assign %r = %add : i32
  }
  emitrust.return %r : i32
}

// An unused named parameter is `_`-prefixed, never bare (deny-lint clean).
// CHECK-LABEL: fn unused_param(_x: i32) {
// CHECK-NEXT:  }
emitrust.func @unused_param(%arg0: i32)
    attributes {emitrust.param_names = ["x"]} {
  emitrust.return
}

// A body local spelled like the parameter uniquifies to `x_1`; the
// parameter, named first, keeps the bare spelling.
// CHECK-LABEL: fn param_local_clash(x: i32) -> i32 {
// CHECK-NEXT:    let x_1: i32;
// CHECK-NEXT:    x_1 = x;
// CHECK-NEXT:    x_1 + x
// CHECK-NEXT:  }
emitrust.func @param_local_clash(%arg0: i32) -> i32
    attributes {emitrust.param_names = ["x"]} {
  %v = emitrust.variable named "x" : !emitrust.lvalue<i32>
  emitrust.assign %v = %arg0 : !emitrust.lvalue<i32>
  %l = emitrust.load %v : (!emitrust.lvalue<i32>) -> i32
  %s = emitrust.add %l, %arg0 : i32
  emitrust.return %s : i32
}

// A method consumes argument 0 as the receiver: its slot (whatever it
// says) is ignored and `self` stays hard-wired; the remaining slots bind
// normally.
// CHECK: impl Counter {
// CHECK-NEXT: fn bump(&mut self, amount: i32) {
// CHECK-NEXT:    (*self).n = amount;
// CHECK-NEXT:  }
emitrust.impl "Counter" {
  emitrust.func @bump(%arg0: !emitrust.mut_ref<!emitrust.struct<"Counter">>,
                      %arg1: i32)
      attributes {emitrust.param_names = ["ignored", "amount"]} {
    %s = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
    %m = emitrust.member %s["n"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
    emitrust.assign %m = %arg1 : !emitrust.lvalue<i32>
    emitrust.return
  }
}

// No attribute: v-numbering exactly as before.
// CHECK-LABEL: fn no_attr(v0: i32) -> i32 {
// CHECK-NEXT:    v0
// CHECK-NEXT:  }
emitrust.func @no_attr(%arg0: i32) -> i32 {
  emitrust.return %arg0 : i32
}
