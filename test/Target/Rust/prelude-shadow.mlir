// FR-150: an emitted TYPE item whose name collides with a Rust PRELUDE name
// shadows that name for the whole crate, and every prelude spelling the
// EMITTER writes then resolves to the user's type instead. This file pins the
// fix: in a crate that shadows a prelude name, the emitter's own uses of that
// name are written as a fully qualified path, while the user's type keeps its
// bare emitted spelling.
//
// Why this is a correctness pin and not cosmetics: the pre-fix emitter wrote
// `Option<fn(i32) -> i32>` beside a `struct Option { .. }` and produced an
// EXIT-0 crate that cannot build (rustc E0107 "struct takes 0 generic
// arguments but 1 generic argument was supplied", E0599 "no method named
// `expect` found for struct `Option`"). No diagnostic, no located rejection --
// the FR-140/141/142/146 class of silent unbuildable output. 123 of the 158
// systemd build failures were this one collision on `src/shared/options.h`'s
// `} Option;`.
//
// FOUR prelude names are covered, because those are the four the emitter
// genuinely writes (`Result` it never writes today):
//   Option -- every fn-ptr PARAMETER, STRUCT FIELD, GLOBAL and temporary
//             (`Option<fn(..)>`), plus the `Option<i64>`/`Option<usize>`
//             importer opaques;
//   Vec    -- the C99-43 C3 argv table (`&[Vec<i8>]`), the FR-94 owned FAM
//             tail, the `Vec::new()` default, and the `Vec<u8>` inside the
//             emitter's own verbatim runtime helpers;
//   String -- the FR-64 malloc-string-fill binding and its `String::new()`
//             default;
//   Box    -- the W2.21 `std::unique_ptr` opaque and its `Box::new` call.
//
// The C spelling is NOT what decides: emitted type names go through
// `toUpperCamelCase`, which DROPS underscores, so C's `my_option` is safe
// (`MyOption`) while `option`, `OPTION` and `Option` all collide. Detection is
// therefore on the EMITTED name, which is what this module carries.
//
// Two things must NOT move, and each is pinned below:
//   * the user's own type keeps its bare name everywhere it is spelled -- the
//     struct_def, the `let` annotation, the struct literal, `::default()`, and
//     an `emitrust.call_opaque` naming a USER static method (`Node::default`)
//     rides the same emission path as the `Box::new` that DOES get qualified,
//     so a blanket rewrite of call callees would break it;
//   * FR-133's unwrapped fn-ptr LOCAL carries no `Option` at all, so it must
//     stay byte-identical (`let cp: fn(i32) -> i32 = addc;`) -- qualification
//     may not leak into a spelling the wrapper was already dropped from.
//
// test/Target/Rust/prelude-shadow-neutral.mlir is the negative half: the same
// shapes in a module that shadows NOTHING must emit the bare prelude names,
// byte for byte as before this FR.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s --strict-whitespace

// The four shadowing type items. Each is an ordinary emitted struct and must
// print with its bare name.
// CHECK:      #[derive(Clone, Copy, Default)]
// CHECK-NEXT: struct Option {
// CHECK-NEXT:     id: i32,
// CHECK-NEXT: }
emitrust.struct_def @Option ["id"] [i32]
// CHECK:      struct Vec {
emitrust.struct_def @Vec ["x"] [i32]
// CHECK:      struct String {
emitrust.struct_def @String ["n"] [i32]
// CHECK:      struct Box {
emitrust.struct_def @Box ["w"] [i32]
// CHECK:      struct Node {
emitrust.struct_def @Node ["v"] [i32]

// A fn-ptr STRUCT FIELD: FR-133 deliberately leaves every non-local position
// wrapped, so this is one of the sites that still collides.
// CHECK:      struct Ops {
// CHECK-NEXT:     apply: ::std::option::Option<fn(i32) -> i32>,
// CHECK-NEXT:     tag: i32,
// CHECK-NEXT: }
emitrust.struct_def @Ops ["apply", "tag"] [!emitrust.fn_ptr<(i32) -> i32>, i32]

// A fn-ptr GLOBAL: the type annotation is qualified, the `Some(..)` payload is
// NOT -- `Some` lives in the VALUE namespace and a struct named `Option` does
// not shadow it.
// CHECK:      thread_local! {
// CHECK-NEXT:     static g_hook: std::cell::Cell<::std::option::Option<fn(i32) -> i32>> = const { std::cell::Cell::new(Some(addc)) };
// CHECK-NEXT: }
emitrust.global @g_hook <#emitrust.opaque<"Some(addc)">> : !emitrust.fn_ptr<(i32) -> i32>

// CHECK-LABEL: fn addc(v0: i32) -> i32 {
emitrust.func @addc(%arg0: i32) -> i32 {
  %c = emitrust.constant <1 : i32> : i32
  %r = emitrust.add %arg0, %c : i32
  emitrust.return %r : i32
}

// A fn-ptr PARAMETER and the `.expect` unwrap at its call site -- the exact
// pair the systemd repro died on.
// CHECK-LABEL: fn apply(v0: ::std::option::Option<fn(i32) -> i32>, v1: i32) -> i32 {
// CHECK-NEXT:    v0.expect("null function pointer")(v1)
// CHECK-NEXT:  }
emitrust.func @apply(%arg0: !emitrust.fn_ptr<(i32) -> i32>, %arg1: i32) -> i32 {
  %r = emitrust.call_indirect %arg0(%arg1)
      : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
  emitrust.return %r : i32
}

// FR-133's admitted LOCAL: no `Option` is written at all, so nothing is
// qualified. This line must be byte-identical to the neutral module's.
// CHECK-LABEL: fn fr133_local(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: fn(i32) -> i32 = addc;
// CHECK-NEXT:    let cp: fn(i32) -> i32 = v1;
// CHECK-NEXT:    cp(v0)
// CHECK-NEXT:  }
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

// A local whose fn-ptr wrapper SURVIVES (it is compared against null, an
// FR-133 refusal leg): both the `let` annotation and the synthesized `None`
// default are on the qualified side and the value-namespace `None` is not.
// CHECK-LABEL: fn nullable_local(v0: i32) -> i32 {
// CHECK-NEXT:    let v1: ::std::option::Option<fn(i32) -> i32> = Some(addc);
// CHECK-NEXT:    let cp: ::std::option::Option<fn(i32) -> i32> = v1;
// CHECK-NEXT:    let v2: ::std::option::Option<fn(i32) -> i32> = cp;
// CHECK-NEXT:    let _v4: bool = v2.is_none();
// CHECK-NEXT:    cp.expect("null function pointer")(v0)
// CHECK-NEXT:  }
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

// The three opaque families the importer mints, each in a `let` annotation.
// `Node::default()` is a USER static method printed through the very same
// call-callee path as `Box::new`, and must stay bare.
// CHECK-LABEL: fn opaque_shapes() -> i32 {
// CHECK-NEXT:    let _v0: ::std::string::String = ::std::string::String::new();
// CHECK-NEXT:    let _v1: ::std::vec::Vec<i8> = ::std::vec::Vec::new();
// CHECK-NEXT:    let v2: Node = Node::default();
// CHECK-NEXT:    let _v3: ::std::boxed::Box<Node> = ::std::boxed::Box::new(v2);
// CHECK-NEXT:    0
// CHECK-NEXT:  }
emitrust.func @opaque_shapes() -> i32 {
  %s = emitrust.call_opaque "String::new"() : () -> !emitrust.opaque<"String">
  %v = emitrust.call_opaque "Vec::new"() : () -> !emitrust.opaque<"Vec<i8>">
  %n = emitrust.call_opaque "Node::default"() : () -> !emitrust.struct<"Node">
  %b = emitrust.call_opaque "Box::new"(%n)
      : (!emitrust.struct<"Node">) -> !emitrust.opaque<"Box<Node>">
  %z = emitrust.constant <0 : i32> : i32
  emitrust.return %z : i32
}

// The no-initializer defaults `emitDefaultValue` renders for the same three
// families, plus the `Option<i64>` cursor cell whose default is `None`.
// CHECK-LABEL: fn default_shapes() {
// CHECK-NEXT:    let _v0: ::std::string::String = ::std::string::String::new();
// CHECK-NEXT:    let _v1: ::std::vec::Vec<i8> = ::std::vec::Vec::new();
// CHECK-NEXT:    let _v2: ::std::option::Option<i64> = None;
// CHECK-NEXT:  }
emitrust.func @default_shapes() {
  %s = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"String">>
  %v = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Vec<i8>">>
  %o = emitrust.variable : !emitrust.lvalue<!emitrust.opaque<"Option<i64>">>
  emitrust.return
}

// The C99-43 C3 argv table parameter: its rendering `&[Vec<i8>]` is a literal
// in the emitter, not an opaque spelling, and needs the same qualification.
// CHECK-LABEL: fn c_main(v0: i32, _v1: &[::std::vec::Vec<i8>]) -> i32 {
emitrust.func @c_main(%arg0: i32, %arg1: !emitrust.argv_table) -> i32 {
  emitrust.return %arg0 : i32
}

// The emitter's own verbatim runtime helper: its `Vec<u8>` is emitter-owned
// boilerplate that shadows exactly like the rest.
// CHECK:      fn __emitrust_cstr_out(s: &[i8]) {
// CHECK:          let bytes: ::std::vec::Vec<u8> = s[..end].iter().map(|&b| b as u8).collect();
emitrust.verbatim "fn __emitrust_cstr_out(s: &[i8]) {\0A    use std::io::Write;\0A    let end = s.iter().position(|&b| b == 0).unwrap_or(s.len());\0A    let bytes: Vec<u8> = s[..end].iter().map(|&b| b as u8).collect();\0A    std::io::stdout().write_all(&bytes).expect(\22stdout write failed\22);\0A}"
