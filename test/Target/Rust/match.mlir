// Phase 2 (match + enums): emitrust.switch renders a Rust match with
// literal integer arms and a trailing `_ =>` default arm; emitrust.enum_def
// renders a value-preserving open enum — a #[repr(transparent)] tuple
// struct over the storage integer with one associated constant per
// variant and a Default impl returning the first variant; enum-typed
// variables default to Name::default(); enum values compare with == / !=,
// cast to integers through `.0 as`, and construct from integers through
// the tuple-struct constructor.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      #[repr(transparent)]
// CHECK-NEXT: #[derive(Clone, Copy, PartialEq)]
// CHECK-NEXT: struct Color(i32);
// CHECK-NEXT: impl Color {
// CHECK-NEXT:     const Red: Color = Color(0);
// CHECK-NEXT:     const Green: Color = Color(1);
// CHECK-NEXT:     const Blue: Color = Color(2);
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for Color {
// CHECK-NEXT:     fn default() -> Color { Color::Red }
// CHECK-NEXT: }
emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 2]

// The unused arm-local constants drop (FR-61d); the arm skeleton and case
// patterns are what this pins.
// CHECK-LABEL: fn simple_match(v0: i32) {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        0 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        -4 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @simple_match(%arg0: i32) {
  emitrust.switch %arg0 : i32
  case 0 {
    %0 = emitrust.constant <1 : i32> : i32
  }
  case -4 {
    %1 = emitrust.constant <2 : i32> : i32
  }
  default {
    %2 = emitrust.constant <3 : i32> : i32
  }
  emitrust.return
}

// Case values render interpreted in the discriminator's type. Against a
// usize scrutinee the stored i64 bits print as UNSIGNED decimal: -1 is the
// zero-extended bit pattern of a C `case -1:` on `long` and must print as
// 18446744073709551615 (printing `-1` would not even compile against
// usize); 4294967289 is the zero-extended pattern of an i32 `case -7:`.
// CHECK-LABEL: fn usize_match(v0: usize) {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        18446744073709551615 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        4294967289 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        7 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @usize_match(%arg0: index) {
  emitrust.switch %arg0 : index
  case -1 {
  }
  case 4294967289 {
  }
  case 7 {
  }
  default {
  }
  emitrust.return
}

// Nested statements inside arms: an if and assignments in a case, multiple
// statements in the default arm, and an empty default arm elsewhere.
// FR-61d: the single-use constants inline into the binding and the arm
// assignments (with literal suffixes, consuming v2/v4/v5 as numbering
// gaps); the unused arm constant drops.
// CHECK-LABEL: fn nested_match(v0: i32, v1: bool) {
// CHECK-NEXT:    let mut _v3: i32 = 0i32;
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        1 => {
// CHECK-NEXT:            if v1 {
// CHECK-NEXT:                _v3 = 5i32;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            _v3 = 7i32;
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        2 => {
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @nested_match(%arg0: i32, %arg1: i1) {
  %0 = emitrust.constant <0 : i32> : i32
  %1 = emitrust.let mut %0 : i32
  emitrust.switch %arg0 : i32
  case 1 {
    emitrust.if %arg1 {
      %2 = emitrust.constant <5 : i32> : i32
      emitrust.assign %1 = %2 : i32
    }
  }
  default {
    %3 = emitrust.constant <7 : i32> : i32
    emitrust.assign %1 = %3 : i32
  }
  emitrust.switch %arg0 : i32
  case 2 {
    %4 = emitrust.constant <9 : i32> : i32
  }
  default {
  }
  emitrust.return
}

// A match inside a loop: break and continue stay legal under case arms.
// CHECK-LABEL: fn match_in_loop(v0: i32) {
// CHECK-NEXT:    loop {
// CHECK-NEXT:        match v0 {
// CHECK-NEXT:            1 => {
// CHECK-NEXT:                break;
// CHECK-NEXT:            }
// CHECK-NEXT:            _ => {
// CHECK-NEXT:                continue;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:  }
emitrust.func @match_in_loop(%arg0: i32) {
  emitrust.loop {
    emitrust.switch %arg0 : i32
    case 1 {
      emitrust.break
    }
    default {
      emitrust.continue
    }
  }
  emitrust.return
}

// The enum comparisons feed a sink so their rendering stays pinned (an
// unused pure cmp would drop, FR-61d); the single-use opaque constant
// `Color::Green` inlines as-is into the comparison.
// CHECK-LABEL: fn enum_ops(v0: Color, v1: Color) -> i32 {
// CHECK-NEXT:    let _v2: Color = Color::default();
// CHECK-NEXT:    sink(v0 == v1, v0 != Color::Green);
// CHECK-NEXT:    v0.0 as i32
// CHECK-NEXT:  }
emitrust.func @enum_ops(%arg0: !emitrust.enum<"Color">, %arg1: !emitrust.enum<"Color">) -> i32 {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  %1 = emitrust.constant <#emitrust.opaque<"Color::Green">> : !emitrust.enum<"Color">
  %2 = emitrust.cmp eq, %arg0, %arg1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  %3 = emitrust.cmp ne, %arg0, %1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  emitrust.call_opaque "sink"(%2, %3) : (i1, i1) -> ()
  %4 = emitrust.cast %arg0 : !emitrust.enum<"Color"> to i32
  emitrust.return %4 : i32
}

// An unsigned-underlying enum stores u32; an integer-to-enum cast renders
// the value-preserving tuple-struct constructor around an `as` cast to
// the storage type, and enum_raw contributes `.0` to the place expression
// of the borrow that consumes it.
// CHECK:      #[repr(transparent)]
// CHECK-NEXT: #[derive(Clone, Copy, PartialEq)]
// CHECK-NEXT: struct Mode(u32);
// CHECK-NEXT: impl Mode {
// CHECK-NEXT:     const Off: Mode = Mode(0);
// CHECK-NEXT:     const On: Mode = Mode(1);
// CHECK-NEXT: }
// CHECK-NEXT: impl Default for Mode {
// CHECK-NEXT:     fn default() -> Mode { Mode::Off }
// CHECK-NEXT: }
emitrust.enum_def @Mode ["Off", "On"] [0, 1] {unsigned_underlying}

// CHECK-LABEL: fn enum_from_int(v0: i32) -> u32 {
// CHECK-NEXT:    let mut v1: Mode;
// CHECK-NEXT:    v1 = Mode(v0 as u32);
// CHECK-NEXT:    let v3: &mut u32 = &mut v1.0;
// CHECK-NEXT:    *v3
// CHECK-NEXT:  }
emitrust.func @enum_from_int(%arg0: i32) -> ui32 {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Mode">>
  %1 = emitrust.cast %arg0 : i32 to !emitrust.enum<"Mode">
  emitrust.assign %0 = %1 : !emitrust.lvalue<!emitrust.enum<"Mode">>
  %2 = emitrust.enum_raw %0 : (!emitrust.lvalue<!emitrust.enum<"Mode">>) -> !emitrust.lvalue<ui32>
  %3 = emitrust.addr_of mut %2 : (!emitrust.lvalue<ui32>) -> !emitrust.mut_ref<ui32>
  %4 = emitrust.deref %3 : (!emitrust.mut_ref<ui32>) -> !emitrust.lvalue<ui32>
  %5 = emitrust.load %4 : (!emitrust.lvalue<ui32>) -> ui32
  emitrust.return %5 : ui32
}
