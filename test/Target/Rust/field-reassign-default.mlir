// FR-63 (clippy::field_reassign_with_default): a struct-typed variable whose
// rendering was `let mut s: S = S::default();` followed immediately by stores
// to distinct single-level fields fuses into ONE functional-update literal
// `let s: S = S { f: v, ..S::default() };`. This pins the fuse's exact gate:
// the maximal qualifying PREFIX fuses (a value that reads the variable being
// built ends the prefix and stays a statement); a value reading a DIFFERENT
// struct fuses; nested-member and indexed places never fuse; a deferred-init
// binding is untouched; when the fused fields cover the whole struct the
// `..S::default()` base is dropped (no clippy::needless_update trade); and
// mut-ness is recomputed WITHOUT the fused stores -- a fused-only binding
// loses `mut` (deny(unused_mut) makes a stale one a hard build failure),
// while a binding with a surviving store keeps it.
//
// FR-111 narrows the W2.17 Drop gate: a `has_drop` struct fuses ONLY when
// the prefix covers EVERY field (the rendering then has no functional-update
// base, so no spurious construct+drop of the base S). Any droppy shape that
// would keep a `..S::default()` base -- partial coverage, or a prefix broken
// before coverage -- stays fully un-fused: that base is a whole extra S,
// constructed and DROPPED (W2.17's measured spurious `dtor 0 0`), so the
// fence is a correctness invariant, not a style choice.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

emitrust.struct_def @Point ["x", "y", "z"] [i32, i32, i32]
emitrust.struct_def @Pair ["a", "b"] [i32, i32]
emitrust.struct_def @Holder ["inner", "arr"] [!emitrust.struct<"Pair">, !emitrust.array<2xi32>]

// Single-field fuse over a proper subset of the fields: the base stays, and
// with no surviving mutation the binding loses `mut`.
// CHECK-LABEL: fn single_field(v0: i32) -> i32 {
// CHECK-NEXT:    let p: Point = Point { x: v0, ..Point::default() };
// CHECK-NEXT:    p.x
// CHECK-NEXT:  }
emitrust.func @single_field(%arg0: i32) -> i32 {
  %p = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"Point">>
  %x = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  emitrust.assign %x = %arg0 : !emitrust.lvalue<i32>
  %x2 = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %x2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Multi-field prefix fuse with the stop condition: `p.z`'s value READS the
// variable being built (`p.x + 1`), so the prefix ends there -- x and y fuse
// in program order, the z store stays a statement, and the surviving store
// keeps the binding `mut`.
// CHECK-LABEL: fn prefix_stop() -> i32 {
// CHECK-NEXT:    let mut p: Point = Point { x: 1i32, y: 2i32, ..Point::default() };
// CHECK-NEXT:    p.z = p.x + 1i32;
// CHECK-NEXT:    p.z
// CHECK-NEXT:  }
emitrust.func @prefix_stop() -> i32 {
  %p = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"Point">>
  %x = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %c1 = emitrust.constant <1 : i32> : i32
  emitrust.assign %x = %c1 : !emitrust.lvalue<i32>
  %y = emitrust.member %p["y"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %c2 = emitrust.constant <2 : i32> : i32
  emitrust.assign %y = %c2 : !emitrust.lvalue<i32>
  %z = emitrust.member %p["z"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %x3 = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %x3 : (!emitrust.lvalue<i32>) -> i32
  %c3 = emitrust.constant <1 : i32> : i32
  %n = emitrust.add %l, %c3 : i32
  emitrust.assign %z = %n : !emitrust.lvalue<i32>
  %z2 = emitrust.member %p["z"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %z2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A value that reads a DIFFERENT struct fuses: only reads of the variable
// being built disqualify. Both bindings lose `mut`.
// CHECK-LABEL: fn reads_other(v0: i32) -> i32 {
// CHECK-NEXT:    let outer: Point = Point { x: v0, ..Point::default() };
// CHECK-NEXT:    let inner: Point = Point { x: outer.x + 3i32, ..Point::default() };
// CHECK-NEXT:    inner.x
// CHECK-NEXT:  }
emitrust.func @reads_other(%arg0: i32) -> i32 {
  %o = emitrust.variable named "outer" : !emitrust.lvalue<!emitrust.struct<"Point">>
  %ox = emitrust.member %o["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  emitrust.assign %ox = %arg0 : !emitrust.lvalue<i32>
  %i = emitrust.variable named "inner" : !emitrust.lvalue<!emitrust.struct<"Point">>
  %ix = emitrust.member %i["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %ox2 = emitrust.member %o["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %l = emitrust.load %ox2 : (!emitrust.lvalue<i32>) -> i32
  %c = emitrust.constant <3 : i32> : i32
  %n = emitrust.add %l, %c : i32
  emitrust.assign %ix = %n : !emitrust.lvalue<i32>
  %ix2 = emitrust.member %i["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %ix2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A nested-member place (`h.inner.a`) is not a single-level field of the
// variable: no fuse, byte-identical to the pre-FR-63 rendering.
// CHECK-LABEL: fn nested_place(v0: i32) -> i32 {
// CHECK-NEXT:    let mut h: Holder = Holder::default();
// CHECK-NEXT:    h.inner.a = v0;
// CHECK-NEXT:    h.inner.a
// CHECK-NEXT:  }
emitrust.func @nested_place(%arg0: i32) -> i32 {
  %h = emitrust.variable named "h" : !emitrust.lvalue<!emitrust.struct<"Holder">>
  %in = emitrust.member %h["inner"] : (!emitrust.lvalue<!emitrust.struct<"Holder">>) -> !emitrust.lvalue<!emitrust.struct<"Pair">>
  %a = emitrust.member %in["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %r = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// An indexed place (`h.arr[i]`) never fuses either.
// CHECK-LABEL: fn indexed_place(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut h: Holder = Holder::default();
// CHECK-NEXT:    h.arr[v1 as usize] = v0;
// CHECK-NEXT:    h.arr[v1 as usize]
// CHECK-NEXT:  }
emitrust.func @indexed_place(%arg0: i32, %arg1: i32) -> i32 {
  %h = emitrust.variable named "h" : !emitrust.lvalue<!emitrust.struct<"Holder">>
  %arr = emitrust.member %h["arr"] : (!emitrust.lvalue<!emitrust.struct<"Holder">>) -> !emitrust.lvalue<!emitrust.array<2xi32>>
  %e = emitrust.subscript %arr[%arg1] : (!emitrust.lvalue<!emitrust.array<2xi32>>, i32) -> !emitrust.lvalue<i32>
  emitrust.assign %e = %arg0 : !emitrust.lvalue<i32>
  %r = emitrust.load %e : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A deferred-init binding (`let p; p = ..`) is out of the FUSE's scope: the
// `p.x` store stays a statement instead of becoming a functional-update field.
// (FR-132 separately merges the declaration with its initializing write, which
// is a different fold at a different program point and leaves the refusal --
// the surviving `p.x = v1;` statement -- intact.)
// CHECK-LABEL: fn deferred_untouched(v0: Point, v1: i32) -> i32 {
// CHECK-NEXT:    let mut p: Point = v0;
// CHECK-NEXT:    p.x = v1;
// CHECK-NEXT:    p.x
// CHECK-NEXT:  }
emitrust.func @deferred_untouched(%arg0: !emitrust.struct<"Point">, %arg1: i32) -> i32 {
  %p = emitrust.variable named "p" : !emitrust.lvalue<!emitrust.struct<"Point">>
  emitrust.assign %p = %arg0 : !emitrust.lvalue<!emitrust.struct<"Point">>
  %x = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  emitrust.assign %x = %arg1 : !emitrust.lvalue<i32>
  %x2 = emitrust.member %p["x"] : (!emitrust.lvalue<!emitrust.struct<"Point">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %x2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// The fused fields cover the WHOLE struct: the `..Pair::default()` base is
// dropped (it would draw clippy::needless_update), matching the aggregate-
// literal style.
// CHECK-LABEL: fn all_fields(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let q: Pair = Pair { a: v0, b: v1, };
// CHECK-NEXT:    q.a
// CHECK-NEXT:  }
emitrust.func @all_fields(%arg0: i32, %arg1: i32) -> i32 {
  %q = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.struct<"Pair">>
  %a = emitrust.member %q["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %b = emitrust.member %q["b"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %b = %arg1 : !emitrust.lvalue<i32>
  %a2 = emitrust.member %q["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %a2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// A fused value whose rendered text is exactly the field's name (here the
// parameter `a` stored into field `a`) collapses to the field SHORTHAND, so
// the fuse cannot introduce clippy::redundant_field_names.
// CHECK-LABEL: fn shorthand(a: i32) -> i32 {
// CHECK-NEXT:    let q: Pair = Pair { a, ..Pair::default() };
// CHECK-NEXT:    q.a
// CHECK-NEXT:  }
emitrust.func @shorthand(%arg0: i32) -> i32
    attributes {emitrust.param_names = ["a"]} {
  %q = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.struct<"Pair">>
  %a = emitrust.member %q["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %a2 = emitrust.member %q["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %a2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// Mut-ness with a SURVIVING store: `q.b = q.a` reads the variable (prefix
// stops before it), so it stays a statement and the binding keeps `mut`.
// CHECK-LABEL: fn keeps_mut(v0: i32, _v1: i32) -> i32 {
// CHECK-NEXT:    let mut q: Pair = Pair { a: v0, ..Pair::default() };
// CHECK-NEXT:    q.b = q.a;
// CHECK-NEXT:    q.b
// CHECK-NEXT:  }
emitrust.func @keeps_mut(%arg0: i32, %arg1: i32) -> i32 {
  %q = emitrust.variable named "q" : !emitrust.lvalue<!emitrust.struct<"Pair">>
  %a = emitrust.member %q["a"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %s = emitrust.load %a : (!emitrust.lvalue<i32>) -> i32
  %b = emitrust.member %q["b"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  emitrust.assign %b = %s : !emitrust.lvalue<i32>
  %b2 = emitrust.member %q["b"] : (!emitrust.lvalue<!emitrust.struct<"Pair">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %b2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

emitrust.struct_def @Noisy ["a", "b"] [i32, i32] {emitrust.has_drop}

// FR-111: a has_drop struct with FULL field coverage fuses base-free -- the
// rendering is byte-identical to the non-droppy all_fields shape (including
// the lost `mut` and the trailing `, `), so no `..Noisy::default()` base
// ever exists to be spuriously dropped.
// CHECK-LABEL: fn droppy_all_fields(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let d: Noisy = Noisy { a: v0, b: v1, };
// CHECK-NEXT:    d.a
// CHECK-NEXT:  }
emitrust.func @droppy_all_fields(%arg0: i32, %arg1: i32) -> i32 {
  %d = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"Noisy">>
  %a = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %b = emitrust.member %d["b"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %b = %arg1 : !emitrust.lvalue<i32>
  %a2 = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %a2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// FR-111 / W2.17 fence: PARTIAL coverage of a has_drop struct must stay
// fully un-fused (no partial literal, no base) -- a kept base would be a
// spurious construct+drop of a whole extra Noisy.
// CHECK-LABEL: fn droppy_partial(v0: i32) -> i32 {
// CHECK-NEXT:    let mut d: Noisy = Noisy::default();
// CHECK-NEXT:    d.a = v0;
// CHECK-NEXT:    d.a
// CHECK-NEXT:  }
emitrust.func @droppy_partial(%arg0: i32) -> i32 {
  %d = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"Noisy">>
  %a = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %a2 = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %a2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}

// FR-111 / W2.17 fence: a droppy prefix broken BEFORE full coverage (the
// repeated `d.a` ends the prefix at one of two fields) stays fully un-fused
// too -- coverage is judged on the fusable prefix, not on the whole body.
// CHECK-LABEL: fn droppy_repeat(v0: i32, v1: i32) -> i32 {
// CHECK-NEXT:    let mut d: Noisy = Noisy::default();
// CHECK-NEXT:    d.a = v0;
// CHECK-NEXT:    d.a = v1;
// CHECK-NEXT:    d.b = v0;
// CHECK-NEXT:    d.b
// CHECK-NEXT:  }
emitrust.func @droppy_repeat(%arg0: i32, %arg1: i32) -> i32 {
  %d = emitrust.variable named "d" : !emitrust.lvalue<!emitrust.struct<"Noisy">>
  %a = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a = %arg0 : !emitrust.lvalue<i32>
  %a3 = emitrust.member %d["a"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %a3 = %arg1 : !emitrust.lvalue<i32>
  %b = emitrust.member %d["b"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  emitrust.assign %b = %arg0 : !emitrust.lvalue<i32>
  %b2 = emitrust.member %d["b"] : (!emitrust.lvalue<!emitrust.struct<"Noisy">>) -> !emitrust.lvalue<i32>
  %r = emitrust.load %b2 : (!emitrust.lvalue<i32>) -> i32
  emitrust.return %r : i32
}
