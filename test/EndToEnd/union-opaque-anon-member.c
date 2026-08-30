// REQUIRES: cargo
// FR-167 differential end-to-end test: the ANONYMOUS spelling of a
// non-flattening union member. `struct S { u64 cmd; union { struct A5;
// struct B5; }; u64 spare[2]; }` is the systemd/ioctl command-block
// shape; its arms are aggregates of differing mapped types, so the
// C11-6.7.2.1p13 one-slot flattening cannot model it and the whole
// RECORD used to die with `unsupported: union type` — while the byte-for
// -byte identical NAMED spelling (`} u;`) imported fine as an FR-78
// opaque blob. This pins that the anonymous spelling now travels the same
// path and stays byte-exact through the three things that make the blob
// observable: arm WRITES through the implicit anonymous hop, a
// WHOLE-STRUCT copy that carries the blob, and a CROSS-ARM PUN READ
// (`status.c` over the low half of `start.a`) served by FR-83's byte
// view. Every seed derives from argc so constant folding cannot hide a
// miscompile, and every observable value is printed and byte-diffed
// against the clang-built native.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/union_opaque_anon_member > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native 7 8 > %t.native3.out
// RUN: %t.crate/target/release/union_opaque_anon_member 7 8 > %t.rust3.out
// RUN: diff %t.native3.out %t.rust3.out

int printf(const char *, ...);

struct A5 {
  unsigned long long a;
  unsigned long long b;
}; /* 16 bytes */

struct B5 {
  unsigned int c;
}; /* 4 bytes */

struct S {
  unsigned long long cmd;
  union {
    struct A5 start;
    struct B5 status;
  }; /* anonymous: no name to select, and no one-slot model either */
  unsigned long long spare[2];
};

struct S g; /* zero-init global carrying the blob */

int main(int argc, char **argv) {
  unsigned long long seed = (unsigned long long)argc;
  g.cmd = 7 * seed;
  g.start.a = 0x1122334455667788ULL + seed;
  g.start.b = 99 + seed;
  g.spare[0] = seed;
  g.spare[1] = seed * 2;
  struct S t = g; /* whole-struct copy: the blob travels as bytes */
  /* `status.c` reads the low half of the bytes `start.a` stored: a
     cross-arm pun read through the blob's byte view. */
  printf("%llu %llu %llu %u %llu %llu\n", t.cmd, t.start.a, t.start.b,
         (unsigned)t.status.c, t.spare[0], t.spare[1]);
  return 0;
}
