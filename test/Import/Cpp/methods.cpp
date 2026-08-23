// RUN: emitrust-import-c %s | FileCheck %s

// W2.2: genuine non-virtual C++ class methods driven onto the existing
// emitrust.impl / emitrust.method_of / MethodCallOp surface (proved
// additive-only by a throwaway risk-gate spike, spike-methodcall). Pins,
// at the raw import level (func.func/func.call, pre convert-func-to-
// emitrust): a mutating method (`&mut self`, mut_ref receiver), a const
// method (`&self`, plain ref receiver — NOT mut_ref), a static method (no
// receiver, `emitrust.static_method` marker, a qualified call-site form),
// two constructors (a default ctor and a parameterized ctor, BOTH using a
// member-initializer list `: value(...)`, never a body assignment — this
// exercises `CXXCtorInitializer` lowering, which the spike's own gold
// program never exercised), field access via `this` (both the implicit
// form threaded through an unqualified member reference and the explicit
// `this->member` spelling), and same-class/cross-class method-name
// overload resolution.
//
// PINNED per-class method mangling scheme (binding; fixes the flat-C-name
// collision the risk-gate spike hit: two classes each declaring `get`
// clobber one another in today's single flat `functions` name map, and
// the spike's constructor was hard-coded to the literal name "new" for
// its one class only):
//
//   <StructName>_<methodBaseName>[_<overloadSuffix>]
//
// * `<StructName>` is the class's already-assigned emitrust struct name
//   (the same name used in `struct_def`/`method_of`).
// * `<methodBaseName>` is the C++ method's identifier, passed through the
//   existing `mangleMemberName` keyword-escape exactly like a struct field
//   (a method named e.g. `type` mangles to `type_` first). A constructor
//   has no ordinary identifier (`CXXConstructorName` is a special
//   `DeclarationName` kind) and uses the fixed base name `new`.
// * `<overloadSuffix>` is present ONLY when the class declares more than
//   one method (or constructor) sharing the same `<methodBaseName>` (a
//   genuine C++ overload set). When present, it is `_` followed by the
//   concatenation, in declaration parameter order, of each parameter's
//   type code (this wave only exercises `i` for `i32`; the table extends
//   as future waves need more codes, but must stay deterministic). A
//   zero-parameter member of an overload set contributes an EMPTY code
//   string, so it keeps the bare `<StructName>_<methodBaseName>` form
//   with no trailing suffix — the single-overload and the zero-arg-
//   overload cases are indistinguishable by symbol, which is fine since a
//   zero-parameter C++ signature is unique within its overload set by
//   construction.
//
// Concretely (proving BOTH collision axes get distinct symbols): `Counter`
// declares two overloads of `get` — the plain call-site pins below are
// literally named `Counter_get` (0 args) and `Counter_get_i` (1 `int`
// arg) — this exact pair is the wave's binding worked example. `Other`
// separately declares its own unrelated `get`, which lands on
// `Other_get`: the struct-name prefix alone keeps it from colliding with
// `Counter_get` even though clang gives both methods the identical
// unqualified name `get`. `Counter`'s two constructors overload on arity
// exactly like `get`: `Counter_new` (default, 0 args) and `Counter_new_i`
// (1 `int` arg).
//
// A static method call site is NOT an ordinary `emitrust.method_call` (it
// has no receiver to borrow): it is pinned as a direct
// `emitrust.call_opaque` naming the FULLY QUALIFIED path
// "<StructName>::<mangled-callee-symbol>" — e.g. `"Counter::Counter_origin"`.
// This intentionally reuses the already-mangled MLIR symbol on BOTH sides
// of the `::` (rather than recovering the original unmangled spelling)
// so the qualified string stays unambiguous and deterministic at the
// import surface. FR-110: the redundancy no longer reaches the emitted
// Rust -- every genuine C++ method (ctor included, dtor excluded: W2.17's
// `drop` rename owns that member) ALSO carries its in-impl spelling as
// the `emitrust.method_rust_name` StringAttr (the mangled symbol minus
// the `<Struct>_` prefix, overload suffix kept: `origin`, `get_i`), and
// the Rust emitter strips at PRINT time only -- `Counter::origin()`,
// `c.get()`, `fn inc` -- while the module symbol namespace, every
// analysis, and every ledger key keep joining on the mangled names
// pinned below.
//
// Constructor lowering shape (binding; the spike's Counter used a plain
// body assignment `{ value = start; }`, never a member-initializer list,
// so this is new ground this wave pins deliberately): a constructor is a
// void `&mut self` method (never returns a value); each `CXXCtorInitializer`
// for a data member — in declaration order, ahead of the constructor's
// own compound-statement body — lowers to an ordinary member assignment
// against the (always-mutable) receiver: `deref(self)`, `member(field)`,
// `assign`. No implicit zero-fill precedes it (this wave's classes never
// mix a member-initializer list with fields it omits).

class Counter {
public:
  Counter() : value(0) {}
  Counter(int start) : value(start) {}
  void inc(int d) { value = value + d; }
  int get() const { return value; }
  int get(int offset) const { return this->value + offset; }
  static int origin() { return 0; }

private:
  int value;
};

class Other {
public:
  int get() const { return v; }

private:
  int v;
};

int use_counters(void) {
  Counter c(5);
  Counter c2;
  c.inc(3);
  c.inc(1);
  int a = c.get();
  int b = c.get(10);
  int o = Counter::origin();
  Other other;
  int ov = other.get();
  return a + b + o + ov + c2.get();
}

// The struct and all its methods import lazily at the class's first use
// (mirroring cpp-basics.cpp's struct-import-order pin), landing as
// top-level ops right before `use_counters`, the only function that
// touches either class. `Counter`'s single-field layout:
// CHECK: emitrust.struct_def @Counter ["value"] [i32]

// Default constructor: no parameters beyond the receiver, member-init
// list `: value(0)` lowers to an ordinary member assignment ahead of the
// (empty) body. Overloaded against the parameterized ctor below, but a
// zero-parameter overload keeps the bare name.
// CHECK-LABEL: func.func @Counter_new
// CHECK-SAME: (%[[NDSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Counter">>)
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "new"
// CHECK: %[[NDRCV:.*]] = emitrust.deref %[[NDSELF]] : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[NDFLD:.*]] = emitrust.member %[[NDRCV]]["value"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
// CHECK: %[[NDZERO:.*]] = arith.constant 0 : i32
// CHECK: emitrust.assign %[[NDFLD]] = %[[NDZERO]] : !emitrust.lvalue<i32>
// CHECK: return

// Parameterized constructor: one `int` parameter overloads it against the
// default ctor above, so it gets the `_i` suffix. `: value(start)` lowers
// the same way, binding the incoming parameter instead of a constant.
// CHECK-LABEL: func.func @Counter_new_i
// CHECK-SAME: (%[[NISELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Counter">>, %[[START:.*]]: i32)
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "new_i"
// CHECK: %[[NIRCV:.*]] = emitrust.deref %[[NISELF]] : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[NIFLD:.*]] = emitrust.member %[[NIRCV]]["value"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[NIFLD]] = %[[START]] : !emitrust.lvalue<i32>
// CHECK: return

// Mutating method: sole "inc" in the class, so it keeps the bare name
// despite taking a parameter (no overload set to disambiguate within).
// `value = value + d;` is an implicit `this->value` read-modify-write.
// CHECK-LABEL: func.func @Counter_inc
// CHECK-SAME: (%[[INCSELF:.*]]: !emitrust.mut_ref<!emitrust.struct<"Counter">>, %[[D:.*]]: i32)
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "inc"
// CHECK: %[[INCRCV:.*]] = emitrust.deref %[[INCSELF]] : (!emitrust.mut_ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[INCFLD:.*]] = emitrust.member %[[INCRCV]]["value"] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<i32>
// CHECK: emitrust.assign %[[INCFLD]] = %{{.*}} : !emitrust.lvalue<i32>

// Const method, zero-arg overload of `get`: the receiver is a plain
// `!emitrust.ref`, NOT `!emitrust.mut_ref` — this is the const-receiver
// pin. Bare name (empty parameter type-code suffix).
// CHECK-LABEL: func.func @Counter_get
// CHECK-SAME: (%[[GSELF:.*]]: !emitrust.ref<!emitrust.struct<"Counter">>) -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "get"
// CHECK: %[[GRCV:.*]] = emitrust.deref %[[GSELF]] : (!emitrust.ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: emitrust.member %[[GRCV]]["value"]

// Const method, one-`int`-arg overload of `get`: same const receiver
// shape, `_i` suffix from the single `int` parameter. Body uses the
// EXPLICIT `this->value` spelling (as opposed to `Counter_inc`'s implicit
// form above) — both must resolve to the identical deref-then-member
// place shape.
// CHECK-LABEL: func.func @Counter_get_i
// CHECK-SAME: (%[[GISELF:.*]]: !emitrust.ref<!emitrust.struct<"Counter">>, %[[OFFSET:.*]]: i32) -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "get_i"
// CHECK: %[[GIRCV:.*]] = emitrust.deref %[[GISELF]] : (!emitrust.ref<!emitrust.struct<"Counter">>) -> !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: emitrust.member %[[GIRCV]]["value"]

// Static method: no receiver argument at all, and the `static_method`
// marker sits alongside `method_of` in the attribute dict.
// CHECK-LABEL: func.func @Counter_origin() -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Counter", emitrust.method_rust_name = "origin", emitrust.static_method}
// CHECK: arith.constant 0 : i32

// `Other`'s independent, differently-owned `get` — distinct symbol from
// `Counter_get` purely through the struct-name prefix, even though both
// are zero-arg const methods named `get`.
// CHECK: emitrust.struct_def @Other ["v"] [i32]
// CHECK-LABEL: func.func @Other_get
// CHECK-SAME: (%[[OSELF:.*]]: !emitrust.ref<!emitrust.struct<"Other">>) -> i32
// CHECK-SAME: attributes {emitrust.method_of = "Other", emitrust.method_rust_name = "get"
// CHECK: emitrust.member %{{.*}}["v"]

// The driver: every call-site shape in one place.
// CHECK-LABEL: func.func @use_counters
// Parameterized-constructor call site: default-init place, then a
// mutable borrow feeds the "new_i" method as an ordinary method_call.
// CHECK: %[[C:.*]] = emitrust.variable named "c" : !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[CREF0:.*]] = emitrust.addr_of mut %[[C]] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.mut_ref<!emitrust.struct<"Counter">>
// CHECK: call @Counter_new_i(%[[CREF0]], %{{.*}}) {emitrust.method_call}
// Default-constructor call site: same shape, no extra argument.
// CHECK: %[[C2:.*]] = emitrust.variable named "c2" : !emitrust.lvalue<!emitrust.struct<"Counter">>
// CHECK: %[[C2REF:.*]] = emitrust.addr_of mut %[[C2]] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.mut_ref<!emitrust.struct<"Counter">>
// CHECK: call @Counter_new(%[[C2REF]]) {emitrust.method_call}
// Two mutating-method call sites (`c.inc(3)`, `c.inc(1)`): each borrows
// `%[[C]]` mutably again.
// CHECK: %[[CREF1:.*]] = emitrust.addr_of mut %[[C]]
// CHECK: call @Counter_inc(%[[CREF1]], %{{.*}}) {emitrust.method_call}
// CHECK: %[[CREF2:.*]] = emitrust.addr_of mut %[[C]]
// CHECK: call @Counter_inc(%[[CREF2]], %{{.*}}) {emitrust.method_call}
// Const-method call sites borrow `%[[C]]` SHARED (no `mut`), pinning the
// call-site half of the const-receiver contract.
// CHECK: %[[CREF3:.*]] = emitrust.addr_of %[[C]] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.ref<!emitrust.struct<"Counter">>
// CHECK: call @Counter_get(%[[CREF3]]) {emitrust.method_call}
// CHECK: %[[CREF4:.*]] = emitrust.addr_of %[[C]] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.ref<!emitrust.struct<"Counter">>
// CHECK: call @Counter_get_i(%[[CREF4]], %{{.*}}) {emitrust.method_call}
// Static-method call site: the qualified `emitrust.call_opaque` form, no
// receiver operand, no `method_call` tag (there is no receiver borrow to
// tag).
// CHECK: emitrust.call_opaque "Counter::Counter_origin"() : () -> i32
// `Other`'s call site: same shared-borrow shape as `Counter_get`, proving
// the two `get`s never cross-resolve.
// CHECK: %[[OTH:.*]] = emitrust.variable named "other" : !emitrust.lvalue<!emitrust.struct<"Other">>
// CHECK: %[[OTHREF:.*]] = emitrust.addr_of %[[OTH]] : (!emitrust.lvalue<!emitrust.struct<"Other">>) -> !emitrust.ref<!emitrust.struct<"Other">>
// CHECK: call @Other_get(%[[OTHREF]]) {emitrust.method_call}
// Final `c2.get()`: reuses `Counter_get` (the same symbol as `c.get()`
// above) on the SECOND instance, proving per-instance dispatch through a
// shared method symbol.
// CHECK: %[[C2REF2:.*]] = emitrust.addr_of %[[C2]] : (!emitrust.lvalue<!emitrust.struct<"Counter">>) -> !emitrust.ref<!emitrust.struct<"Counter">>
// CHECK: call @Counter_get(%[[C2REF2]]) {emitrust.method_call}
