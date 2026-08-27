// FR-134: the driver end of the optional canonicalization pipe.
//
// `--check-canonical-roundtrip` adds emitrust-canonical-roundtrip as stage 0 of
// the pinned lowering pipeline -- before every lowering stage, so what it
// certifies is the FRONT END's module, the point at which a round-trip failure
// is still attributable to the importer.
//
// THE CONTRACT THIS FILE PINS IS BYTE-INERTNESS: the pass mutates nothing, so
// the emitted Rust must be identical with the flag on and with it off. That is
// what makes the stage safe to offer at all -- turning it on can only pass
// silently or fail the compile loudly, never change the program. Verified here
// by diffing the two emissions of the same input rather than by asserting on
// text, so the pin cannot rot into a weaker check.
//
// RUN: emitrust-cc --emit=rust %s -o %t.off.rs
// RUN: emitrust-cc --check-canonical-roundtrip --emit=rust %s -o %t.on.rs
// RUN: diff %t.off.rs %t.on.rs
//
// The flag is off by default, and does not apply to --link.
// RUN: not emitrust-cc --link --check-canonical-roundtrip %s -o %t.crate 2>&1 \
// RUN:   | FileCheck --check-prefix=LINK %s
// LINK: error: --link takes object/shard files, not C sources
// LINK-SAME: --check-canonical-roundtrip do not apply

int printf(const char *, ...);

// A switch, an unsigned scrutinee and a loop: the shapes that make the
// front end's module worth round-tripping in the first place.
unsigned classify(unsigned u) {
  switch (u) {
  case 0u:
    return 1u;
  case 4000000000u:
    return 2u;
  default:
    return 3u;
  }
}

int main(void) {
  unsigned total = 0;
  for (unsigned i = 0; i < 4u; i++)
    total += classify(i);
  printf("%u\n", total);
  return 0;
}
