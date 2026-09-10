// FR-59 workspace partitioning, driver-level pins (no cargo needed): the
// GLOBALS INVARIANT, the override map, and the no-partition byte-identity.
//
// The invariant: module-level state never crosses a crate boundary. FR-51
// deliberately never exports `emitrust.global` (each is a `static` or a
// `thread_local!` Cell; global names are not a sound linkage oracle), and
// internal-linkage statics are co-located with their users BY CONSTRUCTION
// (the partition unit is a whole TU, and a C file-static cannot be
// referenced from another TU). So when TU gb reads the extern global that
// TU ga defines and the directory rule would split them, the planner
// CONDENSES gb's crate into ga's with a warning -- never a silently
// widened `pub static`.
//
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-partition-global/ga/def.c -o %t.a.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-partition-global/gb/use.c -o %t.b.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
//
// RUN: rm -rf %t.ws
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws --crate-name gapp --emit=crate --partition 2>%t.err
// RUN: FileCheck %s --check-prefix=GLOBAL < %t.err
// GLOBAL: warning: workspace partition: condensing 'gb' into 'ga'
// GLOBAL-SAME: global
// GLOBAL-SAME: SHARED_G
//
// FR-62 F1b: the bin-local partition lift covers only the binary
// member's actors; this corpus's one actor (SHARED_G and its arms) lives
// in the 'ga' lib member, so it demotes with the spans-note and the
// workspace keeps today's demoted form everywhere:
// GLOBAL: warning: actor plan: demoted SHARED_G: spans workspace crates
//
// One lib member (ga absorbed gb), one bin member; the global is private
// in it -- no pub static anywhere in the workspace.
// RUN: ls %t.ws/ga/src/lib.rs %t.ws/gapp/src/main.rs
// RUN: not ls %t.ws/gb
// RUN: grep "SHARED_G" %t.ws/ga/src/lib.rs
// RUN: not grep "pub static" %t.ws/ga/src/lib.rs
// RUN: not grep "pub static" %t.ws/gapp/src/main.rs
//
// Member manifests carry the same deny-lint table as a single crate, and
// the bin member depends on the lib member:
// RUN: FileCheck %s --check-prefix=LINTS < %t.ws/ga/Cargo.toml
// LINTS: [lints.rust]
// LINTS: unused_variables = "deny"
// RUN: FileCheck %s --check-prefix=BINDEP < %t.ws/gapp/Cargo.toml
// BINDEP: [dependencies]
// BINDEP: ga = { path = "../ga" }
//
// FR-220 note, added when the crate-root allow header became EMPTY under the
// idiomatic rename: a DEPENDENT member's header used to be built by extending
// the base header's lint list (`drop_back(")]\n")` + ", unused_imports"),
// which cannot extend an empty string. The member header is now written
// directly, and this pins the result -- one lint, then the glob imports that
// are how the emitter's bare cross-crate names resolve against FR-51's pubs.
// The LIB member depends on nothing, so its root starts at its first item.
// RUN: FileCheck %s --check-prefix=MEMBERROOT --strict-whitespace \
// RUN:   < %t.ws/gapp/src/main.rs
//      MEMBERROOT:#![allow(unused_imports)]
// MEMBERROOT-NEXT:use ga::*;
// RUN: not grep '#!\[allow' %t.ws/ga/src/lib.rs
//
// Override map: assigning BOTH directories to one crate name removes the
// boundary entirely -- one member named `combined`, no condensation
// warning at all.
// RUN: echo "%S/Inputs/link-partition-global combined" > %t.map
// RUN: rm -rf %t.ws2
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.ws2 --crate-name gapp --emit=crate --partition --partition-map %t.map 2>%t.err2
// RUN: not grep "condensing" %t.err2
// RUN: grep "demoted SHARED_G: spans workspace crates" %t.err2
// RUN: ls %t.ws2/combined/src/lib.rs %t.ws2/gapp/src/main.rs
//
// No-partition byte-identity: without --partition the same link emits the
// single crate exactly as before -- crate root byte-identical to the joint
// import's --emit=rust, no workspace members anywhere. Since FR-62 F1a-3
// the defaulted --actor-lift lifts under --link too (every shard carries
// FR-57d graph metadata), so both sides compare in the same LIFTED form;
// the --partition runs above lift only BIN-LOCAL actors (F1b), and this
// corpus has none, so their emitted crates keep today's demoted form.
// RUN: rm -rf %t.single
// RUN: emitrust-cc --link %t.a.o %t.b.o %t.main.o -o %t.single --crate-name gapp --emit=crate
// RUN: emitrust-cc --emit=rust %S/Inputs/link-partition-global/ga/def.c %S/Inputs/link-partition-global/gb/use.c %s -o %t.joint.rs
// RUN: diff %t.joint.rs %t.single/src/main.rs
// RUN: not ls %t.single/ga
//
// Gate: --partition outside --link --emit=crate is a clean error.
// RUN: not emitrust-cc --emit=rust --partition %s -o - 2>&1 | FileCheck %s --check-prefix=GATE
// GATE: error: --partition requires --link and --emit=crate

int geta(void);
int getb(void);

int printf(const char *, ...);

int main(void) {
  printf("a=%d b=%d\n", geta(), getb());
  return 0;
}
