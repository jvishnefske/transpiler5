// The collision the keyword mangle can introduce, and its fallback.
//
// `mangleMemberName`'s contract (CSymbolNaming.h) names this hazard
// explicitly: appending `_` to a keyword can land on a spelling another
// declaration already owns, and such a collision must be REJECTED, never
// silently merged. The actor-lift planner derives its field and local
// spellings from the emitted global symbols, so a TU declaring both `pub`
// and `pub_` derives `pub_` twice.
//
// The planner already had the right safety net for this -- the mangle
// simply makes it reachable. Two owned globals mapping to one field name
// DEMOTES the whole actor with a located-by-symbol warning, and both
// globals keep their thread-local form: distinct cells, distinct
// spellings, no merge. That is a capability floor, not a miscompile, and
// it is pinned here so a future re-spelling cannot quietly turn it into
// one shared cell.
//
// RUN: emitrust-cc --emit=rust %s -o - 2>%t.err | FileCheck %s
// RUN: FileCheck --check-prefix=WARN %s < %t.err

int printf(const char *, ...);

int pub = 5;
int pub_ = 7;

void bump(int n) {
  pub += n;
  pub_ += n * 2;
}

int main(int argc, char **argv) {
  bump(argc);
  printf("%d %d\n", pub, pub_);
  return 0;
}

// WARN: warning: actor plan: demoted PUB: two owned globals map to the same field name

// No actor struct is synthesized; both cells stay thread-local and stay
// DISTINCT.
// CHECK-NOT: struct PubActor
// CHECK: static PUB: std::cell::Cell<i32> = const { std::cell::Cell::new(5) };
// CHECK: static PUB_: std::cell::Cell<i32> = const { std::cell::Cell::new(7) };
// CHECK-NOT: struct PubActor
