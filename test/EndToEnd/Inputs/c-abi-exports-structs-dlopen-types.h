/* FR-182: the ONE definition of every record that crosses the C ABI in the
   c-abi-exports-structs dlopen end-to-end test, included by all three legs --
   the transpiled library source, the clang-built native oracle, and the
   dlopen host.

   Sharing the header is what makes the byte-diff mean something. The HOST
   allocates `struct tflac` and `struct span` with the C compiler's layout and
   hands their addresses to the emitted cdylib; if the Rust image lays the
   same record out differently, the library reads and writes the wrong bytes.
   Two hand-copied definitions could drift into agreement by accident, which
   would hide exactly the defect this test exists to catch. */

#ifndef EMITRUST_C_ABI_EXPORTS_STRUCTS_TYPES_H
#define EMITRUST_C_ABI_EXPORTS_STRUCTS_TYPES_H

/* Two same-width floats: the smallest faithful record, and the one the SysV
   ABI packs into a SINGLE SSE register when passed by value. */
typedef struct {
  float x;
  float y;
} vec2;

/* The FR-181 corpus shape. Mixed widths with INTERIOR PADDING: `cur_blocksize`
   sits at offset 12 and not at 10, so a repr that packs or reorders produces
   a different answer without changing the record's size. */
struct tflac {
  unsigned int blocksize;
  unsigned int samplerate;
  unsigned char channel_mode;
  unsigned char partition_order;
  unsigned int cur_blocksize;
};

/* An INTEGER member, an SSE member and another INTEGER member: passed by
   value this record's eightbytes are classified independently, so a
   reordering repr misassigns the registers. */
struct wide {
  int a;
  double b;
  int c;
};

/* 24 bytes: over the SysV two-eightbyte limit, so it is MEMORY class -- passed
   on the STACK and returned through the hidden sret pointer. */
struct big {
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
};

/* TRANSITIVITY: `span`'s own layout is only clang's if `vec2`'s is. */
struct span {
  vec2 lo;
  vec2 hi;
};

#endif
