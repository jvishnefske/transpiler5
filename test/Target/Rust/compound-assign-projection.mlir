// FR-63 (assign_op_pattern, projection places): the compound-assign fold
// (`p = p <op> e` -> `p <op>= e`) extends past bare variables to PROJECTION
// places -- `x.f`, `x.f.g`, `a[i]`, `*p` -- because the importer materializes
// the LHS place and the RHS load's place as SEPARATE SSA chains, so the fold
// gate compares the chains STRUCTURALLY and demands provable purity: every
// link is member/enum_raw/subscript/deref down to a root variable, and every
// non-place operand (subscript index, deref base) is a name, an equal
// constant, or an inline pure twin (cast chain / load of the same pure
// place). Today such a place renders twice (LHS place + RHS load); the fold
// renders it once -- legal only because re-rendering was effect-free, so
// dropping one render cannot change behaviour (identical overflow semantics
// on the signed/infix path; unsigned `+ - *` render `.wrapping_*` and never
// fold). When identity or purity is UNPROVABLE the assign must keep its
// `p = p <op> e` form: this file pins the fold for each projection shape AND
// the conservative rejections (differing constant indices, twin binary-op
// indices the gate cannot prove equal, and a hoisted non-inline load whose
// binding the fold would orphan).
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @Point ["x", "y"] [i32, i32]
emitrust.struct_def @Inner ["x"] [i32]
emitrust.struct_def @Outer ["inner"] [!emitrust.struct<"Inner">]

// Field place through `&mut` param auto-deref: dual member(deref(%arg0))
// chains fold (the deref base is the same block argument, a name render).
// CHECK-LABEL: fn scale(v0: &mut Point, v1: i32) {
// CHECK-NEXT:    v0.x *= v1;
// CHECK-NEXT:  }
emitrust.func @scale(%arg0: !emitrust.mut_ref<!emitrust.struct<"Point">>, %arg1: i32) {
  %p1 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
  %m1 = emitrust.member %p1["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %p2 = emitrust.deref %arg0 : (!emitrust.mut_ref<!emitrust.struct<"Point">>) -> !emitrust.lvalue<!emitrust.struct<"Point">>
  %m2 = emitrust.member %p2["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %m2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.mul %l, %arg1 : i32
  emitrust.assign %m1 = %n : !emitrust.lvalue<i32>
  emitrust.return
}

// Nested field place over a variable root: member.member chains fold.
// CHECK-LABEL: fn nested() -> i32 {
// CHECK-NEXT:    let mut o: Outer = Outer::default();
// CHECK-NEXT:    o.inner.x += 100i32;
// CHECK-NEXT:    o.inner.x
// CHECK-NEXT:  }
emitrust.func @nested() -> i32 {
  %o = emitrust.variable named "o" : !emitrust.lvalue<!emitrust.struct<"Outer">>
  %i1 = emitrust.member %o["inner"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  %x1 = emitrust.member %i1["x"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<i32>
  %i2 = emitrust.member %o["inner"] : (!emitrust.lvalue<!emitrust.struct<"Outer">>) -> !emitrust.lvalue<!emitrust.struct<"Inner">>
  %x2 = emitrust.member %i2["x"] : (!emitrust.lvalue<!emitrust.struct<"Inner">>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %x2 : (!emitrust.lvalue<i32>) -> i32
  %c = emitrust.constant <100 : i32> : i32
  %n = emitrust.add %l, %c : i32
  emitrust.assign %x1 = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %x1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Index place with a SHARED index value: multi-use, so it renders as a name
// on both sides -- trivially the same read, and the kept render keeps the
// name alive.
// CHECK-LABEL: fn bump(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut a: [i32; 4] = [0; 4];
// CHECK-NEXT:    a[v0 as usize] += v1;
// CHECK-NEXT:    a[v0 as usize]
// CHECK-NEXT:  }
emitrust.func @bump(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>
  %s1 = emitrust.subscript %a[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %s2 = emitrust.subscript %a[%arg0] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %l = emitrust.load %s2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg1 : i32
  emitrust.assign %s1 = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Index place with TWIN single-use inline cast chains over the same source
// (the importer's `a[i]` shape after mem2reg): pure cast twins fold. Also
// covers the signed `-` path.
// CHECK-LABEL: fn bump64(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut a: [i32; 4] = [0; 4];
// CHECK-NEXT:    a[v0 as i64 as usize] -= v1;
// CHECK-NEXT:    a[v0 as i64 as usize]
// CHECK-NEXT:  }
emitrust.func @bump64(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>
  %c1 = emitrust.cast %arg0 : i32 to i64
  %s1 = emitrust.subscript %a[%c1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
  %c2 = emitrust.cast %arg0 : i32 to i64
  %s2 = emitrust.subscript %a[%c2] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i64) -> !emitrust.lvalue<i32>
  %l = emitrust.load %s2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.sub %l, %arg1 : i32
  emitrust.assign %s1 = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Deref place through a `&mut` param: dual derefs of the same reference fold
// (a bare `*p` target needs no parens, matching the unfolded spelling).
// CHECK-LABEL: fn through(v0: &mut i32, v1: i32) {
// CHECK-NEXT:    *v0 += v1;
// CHECK-NEXT:  }
emitrust.func @through(%arg0: !emitrust.mut_ref<i32>, %arg1: i32) {
  %p1 = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %p2 = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %p2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg1 : i32
  emitrust.assign %p1 = %n : !emitrust.lvalue<i32>
  emitrust.return
}

// NOT the same element: differing constant indices (`a[0] = a[1] + e`) must
// keep the plain `=` form -- folding would redirect the read.
// CHECK-LABEL: fn otherindex(v0: i32) -> i32 {
// CHECK-NEXT:    let mut a: [i32; 4] = [0; 4];
// CHECK-NEXT:    a[0i32 as usize] = a[1i32 as usize] + v0;
// CHECK-NEXT:    a[0i32 as usize]
// CHECK-NEXT:  }
emitrust.func @otherindex(%arg0: i32) -> i32 {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>
  %z = emitrust.constant <0 : i32> : i32
  %one = emitrust.constant <1 : i32> : i32
  %s1 = emitrust.subscript %a[%z] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %s2 = emitrust.subscript %a[%one] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %l = emitrust.load %s2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg0 : i32
  emitrust.assign %s1 = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// UNPROVABLE index identity: twin binary-op indices render the same text but
// sit outside the conservative pure-twin set (constants, names, casts,
// loads), so the assign stays in `p = p + e` form -- the gate refuses what
// it cannot prove.
// CHECK-LABEL: fn twinadds(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut a: [i32; 4] = [0; 4];
// CHECK-NEXT:    a[(v0 + 1i32) as usize] = a[(v0 + 1i32) as usize] + v1;
// CHECK-NEXT:    a[(v0 + 1i32) as usize]
// CHECK-NEXT:  }
emitrust.func @twinadds(%arg0: i32, %arg1: i32) -> i32 {
  %a = emitrust.variable named "a" : !emitrust.lvalue<!emitrust.array<4xi32>>
  %c1 = emitrust.constant <1 : i32> : i32
  %i1 = emitrust.add %arg0, %c1 : i32
  %s1 = emitrust.subscript %a[%i1] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %c2 = emitrust.constant <1 : i32> : i32
  %i2 = emitrust.add %arg0, %c2 : i32
  %s2 = emitrust.subscript %a[%i2] : (!emitrust.lvalue<!emitrust.array<4xi32>>, i32) -> !emitrust.lvalue<i32>
  %l = emitrust.load %s2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg1 : i32
  emitrust.assign %s1 = %n : !emitrust.lvalue<i32>
  %r = emitrust.load %s1 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A multi-use load is HOISTED to its own `let` (not inline): folding would
// orphan that binding, so the projection fold, like the bare-variable fold,
// requires the operand-0 load to render inline and keeps `=` here.
// CHECK-LABEL: fn hoisted(v0: &mut i32, v1: i32) -> i32 {
// CHECK-NEXT:    let v2: i32 = *v0;
// CHECK-NEXT:    *v0 = v2 + v1;
// CHECK-NEXT:    v2
// CHECK-NEXT:  }
emitrust.func @hoisted(%arg0: !emitrust.mut_ref<i32>, %arg1: i32) -> i32 {
  %p1 = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %p2 = emitrust.deref %arg0 : (!emitrust.mut_ref<i32>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %p2 : (!emitrust.lvalue<i32>) -> i32
  %n = emitrust.add %l, %arg1 : i32
  emitrust.assign %p1 = %n : !emitrust.lvalue<i32>
  emitrust.return %l : i32
}
