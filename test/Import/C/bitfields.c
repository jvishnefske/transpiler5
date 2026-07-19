// RUN: emitrust-import-c %s | FileCheck %s

// C99-45 (c-testsuite 00218): bit-field members import as synthesized
// mask-and-shift accessors over a backing unsigned integer field. Each
// maximal run of consecutively declared bit-field members packs, in
// declaration order LSB-first, into one backing field of the smallest
// unsigned type (ui8/ui16/ui32/ui64) that holds the run's total bits.
// Backing fields are named __bits0, __bits1, ... per run, in run order;
// runs are split by any non-bit-field member. The individual bit-field
// member names never appear in the struct_def.
//
// READ  = load backing, shr by the field's bit offset (always emitted,
//         even for offset 0), and with the width mask, then convert to
//         the member's mapped type: unsigned/_Bool/enum-typed fields
//         zero-extend (an enum : 8 holding 152 reads back 152, never
//         -104); plain-int signed fields sign-extend from their declared
//         width via shl/shrsi in the mapped signed type.
// WRITE = read-modify-write: load backing, clear the field's window
//         (and with the complement mask), truncate the new value to the
//         width (cast to backing type + and with the width mask), shl by
//         the bit offset (always emitted), or into the cleared word,
//         assign the backing field.

enum Code { SMALL = 7, BIG = 152 };

// One run: 8 + 1 + 1 = 10 bits -> ui16 backing field __bits0.
struct Packed {
  enum Code code : 8;
  unsigned hot : 1;
  unsigned dirty : 1;
};

// One run of 4 + 3 = 7 bits -> ui8 backing.
struct SignedRun {
  int s : 4;
  unsigned pad : 3;
};

// Two runs split by a plain member -> two backing fields.
struct Split {
  unsigned a : 3;
  int mid;
  unsigned b : 5;
};

// CHECK-DAG: emitrust.enum_def @Code ["SMALL", "BIG"] [7, 152] {unsigned_underlying}
// CHECK-DAG: emitrust.struct_def @Packed ["__bits0"] [ui16]
// CHECK-DAG: emitrust.struct_def @SignedRun ["__bits0"] [ui8]
// CHECK-DAG: emitrust.struct_def @Split ["__bits0", "mid", "__bits1"] [ui8, i32, ui8]

// Read of the 8-bit enum field at bit offset 0: shift, mask with 255,
// then a cast chain ending in the enum type. The masked ui16 source is
// unsigned, so the conversion zero-extends: BIG (152, bit 7 set) reads
// back as 152.
enum Code read_code(struct Packed p) {
  return p.code;
}

// CHECK-LABEL: func.func @read_code
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"Packed">) -> !emitrust.enum<"Code">
// CHECK: %[[B:.*]] = emitrust.member %{{.*}}["__bits0"] : (!emitrust.lvalue<!emitrust.struct<"Packed">>) -> !emitrust.lvalue<ui16>
// CHECK: emitrust.load %[[B]] : (!emitrust.lvalue<ui16>) -> ui16
// CHECK: emitrust.shr %{{.*}}, %{{.*}} : ui16
// CHECK-DAG: emitrust.constant <255 : ui16> : ui16
// CHECK: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.cast %{{.*}} to !emitrust.enum<"Code">
// CHECK: return

// Read of a 1-bit flag at bit offset 9: shift by 9, mask with 1, then a
// zero-extending conversion to the member's mapped unsigned type.
unsigned read_dirty(struct Packed p) {
  return p.dirty;
}

// CHECK-LABEL: func.func @read_dirty
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"Packed">) -> ui32
// CHECK: %[[B:.*]] = emitrust.member %{{.*}}["__bits0"]
// CHECK: emitrust.load %[[B]] : (!emitrust.lvalue<ui16>) -> ui16
// CHECK: emitrust.shr %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.cast %{{.*}} : ui16 to ui32
// CHECK: return

// Write of the 1-bit flag at bit offset 8: RMW with the window cleared
// through the complement mask 0xFEFF = 65279.
void set_hot(struct Packed *p) {
  p->hot = 1;
}

// CHECK-LABEL: func.func @set_hot
// CHECK: emitrust.deref
// CHECK: %[[B:.*]] = emitrust.member %{{.*}}["__bits0"] : (!emitrust.lvalue<!emitrust.struct<"Packed">>) -> !emitrust.lvalue<ui16>
// CHECK-DAG: emitrust.load %[[B]] : (!emitrust.lvalue<ui16>) -> ui16
// CHECK-DAG: emitrust.constant <65279 : ui16> : ui16
// CHECK-DAG: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK-DAG: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK-DAG: emitrust.shl %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.or %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.assign %[[B]] = %{{.*}} : !emitrust.lvalue<ui16>

// Write of the enum field at bit offset 0: the enum value converts to the
// backing type, is truncated to 8 bits, and lands under the complement
// mask 0xFF00 = 65280.
void set_code(struct Packed *p, enum Code c) {
  p->code = c;
}

// CHECK-LABEL: func.func @set_code
// CHECK: emitrust.deref
// CHECK: %[[B:.*]] = emitrust.member %{{.*}}["__bits0"] : (!emitrust.lvalue<!emitrust.struct<"Packed">>) -> !emitrust.lvalue<ui16>
// CHECK-DAG: emitrust.cast %{{.*}} : !emitrust.enum<"Code"> to
// CHECK-DAG: emitrust.load %[[B]] : (!emitrust.lvalue<ui16>) -> ui16
// CHECK-DAG: emitrust.constant <65280 : ui16> : ui16
// CHECK-DAG: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK-DAG: emitrust.and %{{.*}}, %{{.*}} : ui16
// CHECK-DAG: emitrust.shl %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.or %{{.*}}, %{{.*}} : ui16
// CHECK: emitrust.assign %[[B]] = %{{.*}} : !emitrust.lvalue<ui16>

// Read of a plain-int signed 4-bit field: after the unsigned mask (15),
// the value converts to the mapped i32 and sign-extends from bit 3 via
// shl 28 / shrsi 28, so a stored 0b1111 reads back as -1.
int read_s(struct SignedRun x) {
  return x.s;
}

// CHECK-LABEL: func.func @read_s
// CHECK-SAME: (%{{.*}}: !emitrust.struct<"SignedRun">) -> i32
// CHECK: %[[B:.*]] = emitrust.member %{{.*}}["__bits0"] : (!emitrust.lvalue<!emitrust.struct<"SignedRun">>) -> !emitrust.lvalue<ui8>
// CHECK: emitrust.load %[[B]] : (!emitrust.lvalue<ui8>) -> ui8
// CHECK: emitrust.shr %{{.*}}, %{{.*}} : ui8
// CHECK-DAG: emitrust.constant <15 : ui8> : ui8
// CHECK: emitrust.and %{{.*}}, %{{.*}} : ui8
// CHECK: emitrust.cast %{{.*}} : ui8 to i32
// CHECK-DAG: arith.constant 28 : i32
// CHECK: arith.shli %{{.*}}, %{{.*}} : i32
// CHECK: arith.shrsi %{{.*}}, %{{.*}} : i32
// CHECK: return

// Non-bit-field members between runs are ordinary fields; the second run
// gets its own backing field with offsets starting again at 0.
int read_split(struct Split w) {
  return w.mid + (int)w.b;
}

// CHECK-LABEL: func.func @read_split
// CHECK-DAG: emitrust.member %{{.*}}["mid"] : (!emitrust.lvalue<!emitrust.struct<"Split">>) -> !emitrust.lvalue<i32>
// CHECK-DAG: emitrust.member %{{.*}}["__bits1"] : (!emitrust.lvalue<!emitrust.struct<"Split">>) -> !emitrust.lvalue<ui8>
// CHECK: return
