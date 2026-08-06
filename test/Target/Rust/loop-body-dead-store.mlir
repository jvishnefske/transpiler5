// FR-61f follow-up: the ONE loop-body dead store that is decidable without the
// forbidden cross-iteration liveness -- a partial store (`v[i] = ..`) whose
// binding is UNCONDITIONALLY overwritten as a whole at the top of every
// iteration and is not live-out of the loop. That top-of-body overwrite is a
// per-iteration kill, so a partial store after the binding's last in-iteration
// read cannot reach the next iteration's reads (they follow the overwrite) nor
// escape the loop; it drops. Everything narrower stays put: a partial store
// that IS read later in the iteration is retained, and -- crucially -- a store
// whose value is consumed ACROSS the back-edge is retained too, because the
// emitter refuses to reason across iterations (that path miscompiled three
// times) and simply keeps any loop-body store lacking the top-of-body kill.
// This pins that emitted loop bodies carry no unused assignment, which is what
// lets `unused_assignments` move from the crate-root allow list to a denied
// lint in Cargo.toml.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// The binding `v1` is wholly reassigned (`v1 = v2`) at the top of the body: the
// per-iteration kill. `v1[0]` is read (`v6 += v1[0]`) so its store stays; the
// trailing `v1[1] = 55` is never read before the next iteration's overwrite and
// is not read after the loop, so it drops entirely.
// CHECK-LABEL: fn loop_dead_store(v0: i32) -> i32 {
// CHECK:         while v4 < v0 {
// CHECK-NEXT:        v1 = v2;
// CHECK-NEXT:        v1[0usize] = v4;
// CHECK-NEXT:        v6 += v1[0usize];
// CHECK-NEXT:        v4 += 1i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v6
// CHECK-NEXT:  }
emitrust.func @loop_dead_store(%arg0: i32) -> i32 {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
  %tmpl = emitrust.load %a : (!emitrust.lvalue<!emitrust.array<3xi32>>) -> !emitrust.array<3xi32>
  %zero = emitrust.constant <0 : i32> : i32
  %i = emitrust.let mut %zero : i32
  %z2 = emitrust.constant <0 : i32> : i32
  %tot = emitrust.let mut %z2 : i32
  emitrust.while {
    %c = emitrust.cmp lt, %i, %arg0 : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    emitrust.assign %a = %tmpl : !emitrust.lvalue<!emitrust.array<3xi32>>
    %i0 = emitrust.constant <0 : index> : index
    %e0 = emitrust.subscript %a[%i0] : (!emitrust.lvalue<!emitrust.array<3xi32>>, index) -> !emitrust.lvalue<i32>
    emitrust.assign %e0 = %i : !emitrust.lvalue<i32>
    %r0 = emitrust.load %e0 : (!emitrust.lvalue<i32>) -> i32
    %nt = emitrust.add %tot, %r0 : i32
    emitrust.assign %tot = %nt : i32
    %i1 = emitrust.constant <1 : index> : index
    %e1 = emitrust.subscript %a[%i1] : (!emitrust.lvalue<!emitrust.array<3xi32>>, index) -> !emitrust.lvalue<i32>
    %d = emitrust.constant <55 : i32> : i32
    emitrust.assign %e1 = %d : !emitrust.lvalue<i32>
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %i, %one : i32
    emitrust.assign %i = %next : i32
    emitrust.yield
  }
  emitrust.return %tot : i32
}

// Guard: NO whole overwrite of `v1` at the top of the body, and `v1[1] = 55` is
// read at the top of the NEXT iteration (`v5 += v1[1]`). Its value is live
// across the back-edge; the emitter does not reason across iterations, so
// finding no top-of-body kill it keeps the store rather than risk a miscompile.
// CHECK-LABEL: fn loop_cross_iter_live(v0: i32) -> i32 {
// CHECK:         while v3 < v0 {
// CHECK-NEXT:        v5 += v1[1usize];
// CHECK-NEXT:        v1[1usize] = 55i32;
// CHECK-NEXT:        v3 += 1i32;
// CHECK-NEXT:    }
// CHECK-NEXT:    v5
// CHECK-NEXT:  }
emitrust.func @loop_cross_iter_live(%arg0: i32) -> i32 {
  %a = emitrust.variable : !emitrust.lvalue<!emitrust.array<3xi32>>
  %zero = emitrust.constant <0 : i32> : i32
  %i = emitrust.let mut %zero : i32
  %z2 = emitrust.constant <0 : i32> : i32
  %tot = emitrust.let mut %z2 : i32
  emitrust.while {
    %c = emitrust.cmp lt, %i, %arg0 : (i32, i32) -> i1
    emitrust.condition %c
  } do {
    %i1 = emitrust.constant <1 : index> : index
    %e1 = emitrust.subscript %a[%i1] : (!emitrust.lvalue<!emitrust.array<3xi32>>, index) -> !emitrust.lvalue<i32>
    %r = emitrust.load %e1 : (!emitrust.lvalue<i32>) -> i32
    %nt = emitrust.add %tot, %r : i32
    emitrust.assign %tot = %nt : i32
    %d = emitrust.constant <55 : i32> : i32
    emitrust.assign %e1 = %d : !emitrust.lvalue<i32>
    %one = emitrust.constant <1 : i32> : i32
    %next = emitrust.add %i, %one : i32
    emitrust.assign %i = %next : i32
    emitrust.yield
  }
  emitrust.return %tot : i32
}
