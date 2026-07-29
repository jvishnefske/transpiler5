// Shared surface of the FR-41 determinism fixture (test/Project/
// coloring-determinism.c). Every symbol here has EXTERNAL linkage on purpose:
// an internal-linkage one would carry the per-TU `tu<i>_` tag, so its node key
// would legitimately change when the translation units are reordered and the
// test could no longer tell a real ordering dependency from that rename.
#ifndef COLORING_DETERMINISM_H
#define COLORING_DETERMINISM_H

/// A Red seed: `_Atomic` is an unconditional `mapType` rejection.
struct Atom {
  _Atomic int cell;
};

/// Poisoned by `Atom` through a `Field` edge.
struct Held {
  struct Atom atom;
  int tag;
};

/// A clean record, so the fixture has a Green item to keep too.
struct Plain {
  int value;
};

/// Defined in the first unit; body-level Red (inline asm), hence stubbable.
void blocked_a(void);
/// Defined in the second unit; body-level Red as well, so a caller of BOTH
/// has two equally good poisoners and the blame choice has to be decided by
/// content (smallest symbol) rather than by which unit was walked first.
void blocked_b(void);

/// Signature-level Red, hence NOT stubbable: its callers go Red.
int wide(struct Held *h);

/// Green in both units.
int plain_value(struct Plain *p);

#endif
