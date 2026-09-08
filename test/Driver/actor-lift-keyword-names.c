// Byte-level golden for the Rust-keyword mangle the FR-62 actor lowerings
// apply to names DERIVED from a C file-scope variable.
//
// The importer's file-scope keyword rejection (globals-keyword.c) is blind
// here: under the FR-53 idiomatic rename `pub` emits as the symbol `PUB`,
// which is not a keyword, and the actor-lift planner snake_cases that
// symbol back to `pub` when it derives a lowered spelling. Both lowerings
// then emitted a bare keyword and the crate failed to PARSE:
//
//   (a) `let mut pub: i32 = 5;`   -- the driver-only global demoted to a
//       named `c_main` local;
//   (b) `struct MatchActor { match: i32 }` plus `self.match`,
//       `MatchActor { match: 7i32 }` and `match_actor.match` -- the
//       multi-function global lifted to an actor field.
//
// Both take `mangleMemberName`'s single trailing underscore, the same
// mangle struct members and keyword-spelled locals already take. This
// test pins the exact spellings at every one of the four use sites the
// actor field appears at (declaration, arm body, driver construction,
// driver read), because a mangle applied at only some of them still
// builds when it is applied consistently to a DIFFERENT wrong name.
//
// `plain` and `total` are the negative controls: a non-keyword global
// keeps its spelling verbatim on both paths, so this golden also pins
// that the mangle does not shift a byte for ordinary names.
//
// --implicit-check-not=thread_local proves both lowerings actually fired
// (a demotion back to the thread-local form would hide the mangle rather
// than exercise it).
// RUN: emitrust-cc --emit=rust %s -o - \
// RUN:   | FileCheck %s --implicit-check-not=thread_local

int printf(const char *, ...);

int pub = 5;
int loop = 11;
int plain = 100;

int match = 7;
int type = 13;
int total = 1000;

int bump(int n) {
  match += n;
  type += n * 2;
  total += match + type;
  return match + type + total;
}

int main(int argc, char **argv) {
  pub += argc;
  loop += argc * 3;
  plain += argc * 5;
  int b = bump(argc);
  printf("locals %d %d %d\n", pub, loop, plain);
  printf("actor  %d %d %d %d\n", match, type, total, b);
  return 0;
}

// The lifted actor's field declarations: keywords mangled, `total` verbatim.
// CHECK:      struct MatchActor {
// CHECK-NEXT:     match_: i32,
// CHECK-NEXT:     total: i32,
// CHECK-NEXT:     type_: i32,
// CHECK-NEXT: }

// The arm body reaches the fields through the mangled spellings.
// CHECK:      fn bump(&mut self, n: i32) -> i32 {
// CHECK-NEXT:     self.match_ += n;
// CHECK-NEXT:     self.type_ += n * 2i32;
// CHECK-NEXT:     self.total += self.match_ + self.type_;
// CHECK-NEXT:     self.match_ + self.type_ + self.total

// The driver constructs the actor with the mangled field names and binds
// each demoted global as a named local -- keywords mangled, `plain`
// verbatim.
// CHECK:      fn c_main(argc: i32) -> i32 {
// CHECK-NEXT:     let mut match_actor: MatchActor = MatchActor { match_: 7i32, total: 1000i32, type_: 13i32, };
// CHECK-NEXT:     let mut loop_: i32 = 11;
// CHECK-NEXT:     let mut plain: i32 = 100;
// CHECK-NEXT:     let mut pub_: i32 = 5;
// CHECK-NEXT:     pub_ += argc;
// CHECK-NEXT:     loop_ += argc * 3i32;
// CHECK-NEXT:     plain += argc * 5i32;

// The driver's reads of the actor fields carry the mangle too.
// CHECK:      println!("actor  {} {} {} {}", match_actor.match_, match_actor.type_, match_actor.total, b);
