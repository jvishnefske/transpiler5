// FR-150 BYTE-IDENTITY GUARD, and the load-bearing half of the pair: a module
// that shadows NO prelude name must emit EXACTLY what it emitted before FR-150
// -- bare `Option<..>`, `Vec<..>`, `String`, `Box<..>`, no qualified path
// anywhere, not one shifted byte.
//
// FR-150 makes qualification CONDITIONAL on a collision precisely so the blast
// radius equals the defect's. Unconditional qualification would shift emitted
// bytes across the whole corpus (`--emit=crate` output is pinned byte-for-byte
// by golden tests, so that is a behavior change, not a cleanup) and would make
// every fn-ptr signature unreadable. This file is the positive statement of
// that: it carries the SAME shapes as test/Target/Rust/prelude-shadow.mlir --
// fn-ptr parameter, fn-ptr struct field, fn-ptr global, the `String`/`Vec<..>`/
// `Box<..>` opaques with their `::new` calls, the no-initializer defaults, the
// argv table, and the emitter's verbatim runtime helper -- but its type items
// are named `MyOption`/`MyVec`/`MyString`/`MyBox`, which is also the point that
// C's `my_option` is SAFE: `toUpperCamelCase` drops the underscore and yields
// `MyOption`, not `Option`.
//
// Every line of the crate is pinned full-width under --strict-whitespace, and a
// second unanchored scan says outright that no qualified prelude path appears
// anywhere in the output -- a CHECK-NOT only guards the span between its
// neighbours, so the full-line pin cannot say that on its own.
// RUN: emitrust-translate --mlir-to-rust %s \
// RUN:   | FileCheck %s --strict-whitespace --match-full-lines
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --check-prefix=BARE

emitrust.struct_def @MyOption ["id"] [i32]
emitrust.struct_def @MyVec ["x"] [i32]
emitrust.struct_def @MyString ["n"] [i32]
emitrust.struct_def @MyBox ["w"] [i32]
emitrust.struct_def @Node ["v"] [i32]
emitrust.struct_def @Ops ["apply", "tag"] [!emitrust.fn_ptr<(i32) -> i32>, i32]
emitrust.global @g_hook <#emitrust.opaque<"Some(addc)">> : !emitrust.fn_ptr<(i32) -> i32>

emitrust.func @addc(%arg0: i32) -> i32 {
  %c = emitrust.constant <1 : i32> : i32
  %r = emitrust.add %arg0, %c : i32
  emitrust.return %r : i32
}

emitrust.func @apply(%arg0: !emitrust.fn_ptr<(i32) -> i32>, %arg1: i32) -> i32 {
  %r = emitrust.call_indirect %arg0(%arg1)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

emitrust.func @fr133_local(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

emitrust.func @nullable_local(%arg0: i32) -> i32 {
  %cp = emitrust.variable named "cp"
      : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %s = emitrust.constant <#emitrust.opaque<"Some(addc)">>
      : !emitrust.fn_ptr<(i32) -> i32>
  emitrust.assign %cp = %s : !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
  %l = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %n = emitrust.constant <#emitrust.opaque<"None">>
      : !emitrust.fn_ptr<(i32) -> i32>
  %eq = emitrust.cmp eq, %l, %n
      : (!emitrust.fn_ptr<(i32) -> i32>, !emitrust.fn_ptr<(i32) -> i32>) -> i1
  %l2 = emitrust.load %cp
      : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>)
      -> !emitrust.fn_ptr<(i32) -> i32>
  %r = emitrust.call_indirect %l2(%arg0)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

emitrust.func @opaque_shapes() -> i32 {
  %s = emitrust.call_opaque "String::new"() : () -> !emitrust.opaque<"String">
  %v = emitrust.call_opaque "Vec::new"() : () -> !emitrust.opaque<"Vec<i8>">
  %n = emitrust.call_opaque "Node::default"() : () -> !emitrust.struct<"Node">
  %b = emitrust.call_opaque "Box::new"(%n)
      : (!emitrust.struct<"Node">) -> !emitrust.opaque<"Box<Node>">
  %z = emitrust.constant <0 : i32> : i32
  emitrust.return %z : i32
}

emitrust.func @default_shapes() {
  %s = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"String">>
  %v = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Vec<i8>">>
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Option<i64>">>
  emitrust.return
}

emitrust.func @c_main(%arg0: i32, %arg1: !emitrust.argv_table) -> i32 {
  emitrust.return %arg0 : i32
}

emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {\0A    use std::io::Write;\0A    let end = s.iter().position(|&b| b == 0).unwrap_or(s.len());\0A    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8).collect();\0A    std::io::stdout().write_all(&bytes).expect(\22stdout write failed\22);\0A}"

// CHECK:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyOption {
// CHECK-NEXT:    id: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyVec {
// CHECK-NEXT:    x: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyString {
// CHECK-NEXT:    n: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct MyBox {
// CHECK-NEXT:    w: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct Node {
// CHECK-NEXT:    v: i32,
// CHECK-NEXT:}
// CHECK-NEXT:#[derive(Clone, Copy, Default)]
// CHECK-NEXT:struct Ops {
// CHECK-NEXT:    apply: Option<fn(i32) -> i32>,
// CHECK-NEXT:    tag: i32,
// CHECK-NEXT:}
// CHECK-NEXT:thread_local! {
// CHECK-NEXT:    static g_hook: std::cell::Cell<Option<fn(i32) -> i32>> = const { std::cell::Cell::new(Some(addc)) };
// CHECK-NEXT:}
// CHECK-NEXT:fn addc(v0: i32) -> i32 {
// CHECK-NEXT:    v0 + 1i32
// CHECK-NEXT:}
// CHECK-NEXT:fn apply(v0: Option<fn(i32) -> i32>, v1: i32) -> i32 {
// CHECK-NEXT:    v0.expect("null function pointer")(v1)
// CHECK-NEXT:}
// CHECK-NEXT:fn fr133_local(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: fn(i32) -> i32 = addc;
// CHECK-NEXT:    let cp: fn(i32) -> i32 = v1;
// CHECK-NEXT:    cp(v0)
// CHECK-NEXT:}
// CHECK-NEXT:fn nullable_local(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let cp: Option<fn(i32) -> i32> = v1;
// CHECK-NEXT:    let v2: Option<fn(i32) -> i32> = cp;
// CHECK-NEXT:    let _v4: bool = v2.is_none();
// CHECK-NEXT:    cp.expect("null function pointer")(v0)
// CHECK-NEXT:}
// CHECK-NEXT:fn opaque_shapes() -> i32 {
// CHECK-NEXT:    let _v0: String = String::new();
// CHECK-NEXT:    let _v1: Vec<i8> = Vec::new();
// CHECK-NEXT:    let v2: Node = Node::default();
// CHECK-NEXT:    let _v3: Box<Node> = Box::new(v2);
// CHECK-NEXT:    0
// CHECK-NEXT:}
// CHECK-NEXT:fn default_shapes() {
// CHECK-NEXT:    let _v0: String = String::new();
// CHECK-NEXT:    let _v1: Vec<i8> = Vec::new();
// CHECK-NEXT:    let _v2: Option<i64> = None;
// CHECK-NEXT:}
// CHECK-NEXT:fn c_main(v0: i32, _v1: &[Vec<i8>]) -> i32 {
// CHECK-NEXT:    v0
// CHECK-NEXT:}
// CHECK-NEXT:fn __emitrust_cstr_out(s: &[i8]) {
// CHECK-NEXT:    use std::io::Write;
// CHECK-NEXT:    let end = s.iter().position(|&b| b == 0).unwrap_or(s.len());
// CHECK-NEXT:    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8).collect();
// CHECK-NEXT:    std::io::stdout().write_all(&bytes).expect("stdout write failed");
// CHECK-NEXT:}

// Not one qualified prelude path anywhere in the crate.
// BARE-NOT: ::std::option::
// BARE-NOT: ::std::vec::
// BARE-NOT: ::std::string::
// BARE-NOT: ::std::boxed::
