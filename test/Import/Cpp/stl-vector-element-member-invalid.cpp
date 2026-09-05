// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/array-at.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYAT
// RUN: not emitrust-import-c %t/array-front.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYFRONT
// RUN: not emitrust-import-c %t/array-back.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYBACK
// RUN: not emitrust-import-c %t/array-at-read.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYATREAD
// RUN: not emitrust-import-c %t/map-struct-value.cpp 2>&1 | FileCheck %s --check-prefix=MAPSTRUCT

// FR-196: THE FRONTIER OF THE ELEMENT-PLACE MEMBER WRITE, PINNED AS
// LOCATED REJECTIONS.
//
// FR-196 routes `v[i].f` / `v.at(i).f` / `v.front().f` / `v.back().f`
// through `emitLValue` instead of letting `emitMemberBasePlace`'s `f().m`
// branch stage a copy of the element and DROP the store. The whole point
// of the change is that the failure direction becomes loud: a receiver
// `emitLValue` cannot resolve to a place now reaches ITS located
// rejection instead of quietly emitting a member access on a temporary
// that dead-store elimination then deletes.
//
// This file is where that promise is cashed. Each split below is a
// receiver the new path deliberately does NOT resolve, and each one must
// name a source location and a reason. If a later wave widens one of
// them, the pin MOVES FORWARD -- it never loosens into silence.
//
// The std::array splits are the interesting ones, because `a[i].f = x`
// DOES work after FR-196 (pinned by the byte-diff in
// test/EndToEnd/stl-vector-element-member-write.cpp): `std::array<T, N>`
// maps to `!emitrust.array<NxT>` and the subscript place is the ordinary
// C array one. `at`/`front`/`back` do not, because `emitLValue`'s
// at/front/back branch requires an `!emitrust.opaque<"Vec<...>">`
// receiver. That asymmetry is deliberate scope, not an accident, so it is
// written down here rather than left to be rediscovered.
//
// NOTE ON THE WORDING: "not a recognized std::vector" is the PRE-EXISTING
// diagnostic of that branch -- `int x = a.at(0);` has produced it since
// W2.6 -- and FR-196 makes the member-projection spelling converge on the
// same one instead of the separate "std::array::at is not a recognized
// STL method" text the staged-copy path used to reach. Converging the two
// spellings on ONE diagnostic is the same principle as converging them on
// one place. The text names std::vector rather than the receiver's own
// type, which is worth improving, but changing it is a diagnostic-surface
// change with its own consumers (test/RealWorld's bucket table) and is
// deliberately NOT bundled into a codegen fix.

//--- array-at.cpp
#include <array>
// ARRAYAT: array-at.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: member call receiver is not a recognized std::vector
struct P {
  int x;
};
void use(std::array<P, 2> &a, int n) { a.at(1).x = n; }

//--- array-front.cpp
#include <array>
// ARRAYFRONT: array-front.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: member call receiver is not a recognized std::vector
struct P {
  int x;
};
void use(std::array<P, 2> &a, int n) { a.front().x = n; }

//--- array-back.cpp
#include <array>
// ARRAYBACK: array-back.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: member call receiver is not a recognized std::vector
struct P {
  int x;
};
void use(std::array<P, 2> &a, int n) { a.back().x = n; }

//--- array-at-read.cpp
#include <array>
// The READ spelling rejects at the same place as the write: neither half
// may reach emission on a receiver with no resolvable element place.
// ARRAYATREAD: array-at-read.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: member call receiver is not a recognized std::vector
struct P {
  int x;
};
int use(std::array<P, 2> &a) { return a.at(1).x; }

//--- map-struct-value.cpp
#include <map>
// `m[k].f = v` over a std::map is the map-entry place, and FR-196's
// predicate admits `operator[]` on any std class -- but a struct VALUE
// type is outside the recognized map value set, so the rejection lands
// earlier, at the declaration, and names the value type. Pinned so that
// widening the map value set later is forced to decide about this shape
// on purpose.
// MAPSTRUCT: map-struct-value.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map value type 'struct P' is not in the supported value set
struct P {
  int x;
};
void use(int n) {
  std::map<int, P> m;
  m[1].x = n;
}
