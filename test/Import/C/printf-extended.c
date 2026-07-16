// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *fmt, ...);

int main(void) {
  int d = -42;
  unsigned int u = 4294967295u;
  long l = 5;
  unsigned long ul = 7;

  // Width and flag forms of %d map 1:1 onto Rust format specs: a plain
  // width right-aligns, '-' left-aligns, '0' zero-pads (Rust's zero pad is
  // sign-aware, matching C's "-0042").
  printf("[%5d][%-5d][%05d][%02d]\n", d, d, d, d);
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["[{:5}][{:<5}][{:05}][{:02}]\0A", 0 : index, 1 : index, 2 : index, 3 : index]}

  // x/X/o print the value as unsigned, so the signed argument is cast to
  // ui32 first: a negative argument then prints its two's-complement bit
  // pattern exactly like C ("%x" of -1 is ffffffff).
  printf("%x %X %o %04X %08x\n", d, d, d, d, d);
  // CHECK-COUNT-5: emitrust.cast %{{.*}} : i32 to ui32
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["{:x} {:X} {:o} {:04X} {:08x}\0A", 0 : index, 1 : index, 2 : index, 3 : index, 4 : index]}

  // %u/%lu take the already-unsigned values as-is; %ld/%li print i64; %lx
  // prints a ui64 in hex.
  printf("%u %lu %ld %li %lx\n", u, ul, l, l, ul);
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["{} {} {} {} {:x}\0A", 0 : index, 1 : index, 2 : index, 3 : index, 4 : index]} : (ui32, ui64, i64, i64, ui64) -> ()

  // An argument whose integer type does not match the directive's width is
  // `as`-cast, mirroring C's varargs read of the low bits on x86-64:
  // %d with a size_t (ui64) argument prints its low 32 bits as signed.
  printf("%d\n", sizeof(int));
  // CHECK: emitrust.cast %{{.*}} : ui64 to i32
  // CHECK: emitrust.call_opaque "print!"(%{{.*}}) {args = ["{}\0A", 0 : index]} : (i32) -> ()

  // %c routes the int-promoted argument through the on-demand
  // __emitrust_fmt_c helper (C converts to unsigned char and prints that
  // byte; ASCII-only).
  printf("%c\n", 65);
  // CHECK: %[[C:.*]] = emitrust.call_opaque "__emitrust_fmt_c"(%{{.*}}) : (i32) -> !emitrust.opaque<"char">
  // CHECK: emitrust.call_opaque "print!"(%[[C]]) {args = ["{}\0A", 0 : index]}

  return 0;
}

// The %c helper is emitted once at module level and matches C's
// unsigned-char conversion for ASCII values.
// CHECK: emitrust.verbatim "fn __emitrust_fmt_c(x: i32) -> char
// CHECK-SAME: (x as u8) as char
