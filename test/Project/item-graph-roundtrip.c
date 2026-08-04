// FR-62 F1a: `parseItemGraphText` is `ItemGraph::print`'s exact inverse —
// print(parse(print(g))) == print(g) — over EVERY existing
// --emit=item-graph test input, so the parser provably captures every
// field the printer emits (node kind/def/linkage/tu/loc and all eleven
// edge kinds, including TakesAddressOf/AddressOfGlobal and the
// CallsIndirect `?` target). The pin matters because the link-side actor
// lift (F1a) re-derives its merged graph from stored printer texts: a
// field the parser dropped would silently starve the lift's certification
// facts. The hidden --verify-item-graph-roundtrip flag performs the
// parse + re-print + byte-compare inside the driver and fails the run on
// any mismatch; the final diff additionally pins that the flag itself
// never perturbs the printed output.
//
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-calls.c -o %t.calls
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-types.c -o %t.types
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-address-of.c -o %t.addr
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-field-indirect.c -o %t.fi
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-hosted-sink.c -o %t.sink
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-struct-rename.c -o %t.sr
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-cpp.cpp -o %t.cpp
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %S/item-graph-multi-tu.c %S/Inputs/item-graph-multi-tu-other.c -o %t.multi
// RUN: emitrust-cc --emit=item-graph --verify-item-graph-roundtrip %s -o %t.self
//
// The flag is inert on success: byte-identical to the plain run.
// RUN: emitrust-cc --emit=item-graph %s -o %t.self.plain
// RUN: diff %t.self.plain %t.self

// A local corpus touching both internal linkage (the `tu0_` retag shapes)
// and an indirect call, so this file's own graph exercises the parser arms
// the other inputs might not combine in one text.
static int tally;

static int step(int x) { return x + tally; }

int apply(int (*fn)(int), int seed) { return fn(seed); }

int drive(void) {
  tally += 1;
  return apply(step, 2);
}
