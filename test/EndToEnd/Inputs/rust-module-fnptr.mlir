// FR-159 phase 1 fixture for ../rust-module-fnptr-e2e.c: the module-rendered
// form of rust-module-fnptr-h1.c + -h2.c + that file's own main. Each TU's
// file statics -- its `add1`/`add2` functions, its fn-ptr dispatch TABLE and
// its accumulator TOTAL -- carry the absolute path symbol
// `crate::tu<N>::<leaf>` instead of the flat `tu<N>_<leaf>` tag, which is
// what makes the emitter render them inside `mod tu<N>`.
//
// Hand-authored on purpose: phase 1 sinks only RECORDS (the link-time
// shape-conflict case), so no C input reaches this shape yet -- moving
// functions and globals into modules is phase 3. The fixture exists so the
// module rendering has a real byte-diff oracle NOW rather than only a
// FileCheck golden, because two of its properties are invisible to
// FileCheck-on-emitted-text alone: an fn-ptr payload spelled relatively
// (`tu0::add1`) is rustc E0433 inside `mod tu0`, and a table that silently
// lost its `const { ... }` wrapper still compiles. The `.c` companions are
// the oracle's source of truth; if a future importer change makes this
// fixture unreachable or stale, regenerate it from them.
//
// Excluded from test discovery by config.excludes = ["Inputs"].
module {
  emitrust.func @"crate::tu0::add1"(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
    %0 = emitrust.constant <1 : i32> : i32
    %1 = emitrust.add %arg0, %0 : i32
    emitrust.return %1 : i32
  }
  emitrust.func @"crate::tu0::add2"(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
    %0 = emitrust.constant <2 : i32> : i32
    %1 = emitrust.mul %arg0, %0 : i32
    emitrust.return %1 : i32
  }
  emitrust.global @"crate::tu0::TABLE" <[#emitrust.opaque<"Some(crate::tu0::add1)">, #emitrust.opaque<"Some(crate::tu0::add2)">]> : !emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.global @"crate::tu0::TOTAL" <0 : i32> : i32
  emitrust.func @drive1(%arg0: i32, %arg1: i32) -> i32 attributes {emitrust.param_names = ["i", "v"]} {
    %0 = emitrust.global_load @"crate::tu0::TOTAL" : i32
    %1 = emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>>
    %2 = emitrust.global_load @"crate::tu0::TABLE" : !emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>
    emitrust.assign %1 = %2 : !emitrust.lvalue<!emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>>
    %3 = emitrust.subscript %1[%arg0] : (!emitrust.lvalue<!emitrust.array<2x!emitrust.fn_ptr<(i32) -> i32>>>, i32) -> !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
    %4 = emitrust.load %3 : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>) -> !emitrust.fn_ptr<(i32) -> i32>
    %5 = emitrust.call_indirect %4(%arg1) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
    %6 = emitrust.add %0, %5 : i32
    emitrust.global_store %6, @"crate::tu0::TOTAL" : i32
    %7 = emitrust.global_load @"crate::tu0::TOTAL" : i32
    emitrust.return %7 : i32
  }
  emitrust.func @"crate::tu1::add1"(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
    %0 = emitrust.constant <10 : i32> : i32
    %1 = emitrust.mul %arg0, %0 : i32
    emitrust.return %1 : i32
  }
  emitrust.global @"crate::tu1::TABLE" <[#emitrust.opaque<"Some(crate::tu1::add1)">]> : !emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>
  emitrust.global @"crate::tu1::TOTAL" <100 : i32> : i32
  emitrust.func @drive2(%arg0: i32) -> i32 attributes {emitrust.param_names = ["v"]} {
    %0 = emitrust.constant <0 : i32> : i32
    %1 = emitrust.global_load @"crate::tu1::TOTAL" : i32
    %2 = emitrust.variable : !emitrust.lvalue<!emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>>
    %3 = emitrust.global_load @"crate::tu1::TABLE" : !emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>
    emitrust.assign %2 = %3 : !emitrust.lvalue<!emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>>
    %4 = emitrust.subscript %2[%0] : (!emitrust.lvalue<!emitrust.array<1x!emitrust.fn_ptr<(i32) -> i32>>>, i32) -> !emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>
    %5 = emitrust.load %4 : (!emitrust.lvalue<!emitrust.fn_ptr<(i32) -> i32>>) -> !emitrust.fn_ptr<(i32) -> i32>
    %6 = emitrust.call_indirect %5(%arg0) : (!emitrust.fn_ptr<(i32) -> i32>, i32) -> i32
    %7 = emitrust.add %1, %6 : i32
    emitrust.global_store %7, @"crate::tu1::TOTAL" : i32
    %8 = emitrust.global_load @"crate::tu1::TOTAL" : i32
    emitrust.return %8 : i32
  }
  emitrust.func @c_main(%arg0: i32) -> i32 attributes {emitrust.param_names = ["argc"]} {
    %0 = emitrust.constant <4 : i32> : i32
    %1 = emitrust.constant <1 : i32> : i32
    %2 = emitrust.constant <0 : i32> : i32
    %3 = emitrust.call_opaque "drive1"(%2, %arg0) : (i32, i32) -> i32
    %4 = emitrust.add %arg0, %1 : i32
    %5 = emitrust.call_opaque "drive1"(%1, %4) : (i32, i32) -> i32
    %6 = emitrust.add %arg0, %0 : i32
    %7 = emitrust.call_opaque "drive2"(%6) : (i32) -> i32
    emitrust.call_opaque "println!"(%3, %5, %7) {args = ["{} {} {}", 0 : index, 1 : index, 2 : index]} : (i32, i32, i32) -> ()
    emitrust.return %2 : i32
  }
  emitrust.verbatim "fn main() { std::process::exit(c_main(std::env::args_os().len() as i32)); }"
}
