// RUN: emitrust-import-c %s | FileCheck %s --implicit-check-not='"__emitrust_fmt_c"'

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
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["[{:5}][{:<5}][{:05}][{:02}]", 0 : index, 1 : index, 2 : index, 3 : index]}

  // x/X/o print the value as unsigned, so the signed argument is cast to
  // ui32 first: a negative argument then prints its two's-complement bit
  // pattern exactly like C ("%x" of -1 is ffffffff).
  printf("%x %X %o %04X %08x\n", d, d, d, d, d);
  // CHECK-COUNT-5: emitrust.cast %{{.*}} : i32 to ui32
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["{:x} {:X} {:o} {:04X} {:08x}", 0 : index, 1 : index, 2 : index, 3 : index, 4 : index]}

  // %u/%lu take the already-unsigned values as-is; %ld/%li print i64; %lx
  // prints a ui64 in hex.
  printf("%u %lu %ld %li %lx\n", u, ul, l, l, ul);
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}) {args = ["{} {} {} {} {:x}", 0 : index, 1 : index, 2 : index, 3 : index, 4 : index]} : (ui32, ui64, i64, i64, ui64) -> ()

  // An argument whose integer type does not match the directive's width is
  // `as`-cast, mirroring C's varargs read of the low bits on x86-64:
  // %d with a size_t (ui64) argument prints its low 32 bits as signed.
  printf("%d\n", sizeof(int));
  // CHECK: emitrust.cast %{{.*}} : ui64 to i32
  // CHECK: emitrust.call_opaque "println!"(%{{.*}}) {args = ["{}", 0 : index]} : (i32) -> ()

  // FR-194: %c writes the argument truncated to ONE BYTE through the raw
  // __emitrust_byte_out helper. C converts the argument to unsigned char and
  // writes THAT ONE CHARACTER (C11 7.21.6.1p8) -- one byte for every value
  // 0..255. The __emitrust_fmt_c Display funnel this replaced widened the
  // byte to a Unicode scalar, and `Display for char` writes UTF-8, so every
  // byte >= 0x80 came out as two (a 256-value sweep produced 384 emitted
  // bytes against 256 native, first differing at offset 0x80). The pending
  // format segment flushes around the raw write, so the trailing newline
  // becomes its own bare println!().
  printf("%c\n", 65);
  // CHECK: %[[C:.*]] = arith.trunci %{{.*}} : i32 to i8
  // CHECK: emitrust.call_opaque "__emitrust_byte_out"(%[[C]]) : (i8) -> ()
  // CHECK: emitrust.call_opaque "println!"() {args = []}

  return 0;
}

// The raw byte helper is emitted once at module level and writes on the same
// globally buffered stdout handle print! locks, so ordering holds. The
// Display funnel it replaced is no longer requested at all -- an unused
// helper is an `unused` deny in the emitted crate, which the RUN line's
// whole-output --implicit-check-not enforces.
// CHECK: emitrust.verbatim "fn __emitrust_byte_out(b: i8) {
// CHECK-SAME: write_all(&[b as u8])
