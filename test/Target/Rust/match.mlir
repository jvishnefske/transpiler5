// Phase 2 (match + enums): emitrust.switch renders a Rust match with
// literal integer arms and a trailing `_ =>` default arm; emitrust.enum_def
// renders a #[repr(i32)] enum whose first variant carries #[default];
// enum-typed variables default to Name::default(); enum values compare
// with == / != and cast to integers with `as`.
// RUN: emitrust-translate --mlir-to-rust %s | FileCheck %s

// CHECK:      #[repr(i32)]
// CHECK-NEXT: #[derive(Clone, Copy, PartialEq, Default)]
// CHECK-NEXT: enum Color {
// CHECK-NEXT:     #[default]
// CHECK-NEXT:     Red = 0,
// CHECK-NEXT:     Green = 1,
// CHECK-NEXT:     Blue = 2,
// CHECK-NEXT: }
emitrust.enum_def @Color ["Red", "Green", "Blue"] [0, 1, 2]

// CHECK-LABEL: fn simple_match(v0: i32) {
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        0 => {
// CHECK-NEXT:            let v1: i32 = 1;
// CHECK-NEXT:        }
// CHECK-NEXT:        -4 => {
// CHECK-NEXT:            let v2: i32 = 2;
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            let v3: i32 = 3;
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
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
// CHECK-NEXT:    return;
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
// CHECK-LABEL: fn nested_match(v0: i32, v1: bool) {
// CHECK-NEXT:    let v2: i32 = 0;
// CHECK-NEXT:    let mut v3: i32 = v2;
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        1 => {
// CHECK-NEXT:            if v1 {
// CHECK-NEXT:                let v4: i32 = 5;
// CHECK-NEXT:                v3 = v4;
// CHECK-NEXT:            }
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:            let v5: i32 = 7;
// CHECK-NEXT:            v3 = v5;
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:    match v0 {
// CHECK-NEXT:        2 => {
// CHECK-NEXT:            let v6: i32 = 9;
// CHECK-NEXT:        }
// CHECK-NEXT:        _ => {
// CHECK-NEXT:        }
// CHECK-NEXT:    }
// CHECK-NEXT:    return;
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
// CHECK-NEXT:    return;
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

// CHECK-LABEL: fn enum_ops(v0: Color, v1: Color) -> i32 {
// CHECK-NEXT:    let mut v2: Color = Color::default();
// CHECK-NEXT:    let v3: Color = Color::Green;
// CHECK-NEXT:    let v4: bool = v0 == v1;
// CHECK-NEXT:    let v5: bool = v0 != v3;
// CHECK-NEXT:    let v6: i32 = v0 as i32;
// CHECK-NEXT:    return v6;
// CHECK-NEXT:  }
emitrust.func @enum_ops(%arg0: !emitrust.enum<"Color">, %arg1: !emitrust.enum<"Color">) -> i32 {
  %0 = emitrust.variable : !emitrust.lvalue<!emitrust.enum<"Color">>
  %1 = emitrust.constant <#emitrust.opaque<"Color::Green">> : !emitrust.enum<"Color">
  %2 = emitrust.cmp eq, %arg0, %arg1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  %3 = emitrust.cmp ne, %arg0, %1 : (!emitrust.enum<"Color">, !emitrust.enum<"Color">) -> i1
  %4 = emitrust.cast %arg0 : !emitrust.enum<"Color"> to i32
  emitrust.return %4 : i32
}
