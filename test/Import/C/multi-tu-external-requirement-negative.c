// FR-52 boundaries: the shapes that keep rejecting even under
// --externals-trait, each because the trait cannot express them faithfully.
// (The global-agg arm is the exception: FR-79 and FR-81 flipped the
// struct-typed extern global POSITIVE, so its arm now pins the flip -- the
// pin moved forward, it did not loosen.)
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/addr-taken.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR %s
// RUN: not emitrust-import-c --externals-trait %t/global-addr.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=GLOBAL-ADDR %s
// RUN: emitrust-import-c --externals-trait %t/global-agg.c \
// RUN:   %t/empty.c | FileCheck --check-prefix=GLOBAL-AGG %s
// RUN: not emitrust-import-c --externals-trait %t/variadic.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=VARIADIC %s

//--- empty.c
int unrelated(int v) { return v; }

//--- addr-taken.c
// An address-taken function is emitted as the opaque constant `Some(host_op)`,
// which spells the item out directly. The trait's type parameter cannot reach
// inside that text, so routing the CALL through `E::host_op` would leave the
// fn-pointer reference dangling. The whole symbol therefore keeps rejecting.
// FR-77 moved the rejection FORWARD, from finalizeProject's
// referenced-but-not-defined walk to the address-taking site itself (the
// walk cannot see an opaque `Some(<name>)`, so an initializer-only reference
// used to escape it entirely); the pin follows: same refusal, now located on
// the address-taking expression with the FR-77 wording.
int host_op(int v);

int apply(int (*f)(int), int v) { return f(v); }

int run(int v) { return apply(host_op, v) + host_op(v); }
// ADDR: addr-taken.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: taking the address of undefined function 'host_op'

//--- global-addr.c
// FR-70 revised FR-52's blanket refusal of undefined external OBJECTS: a
// scalar whose every access is a direct whole-value load or store is now a
// getter/setter-pair requirement (multi-tu-external-requirement-global.c).
// The refusal SHRINKS, it never disappears: an ADDRESS-TAKEN global keeps
// rejecting, because `E::host_counter()` yields a VALUE and no trait item
// yields a place `&host_counter` could point at. The address-of is an AST
// fact, not an IR one -- the `&host_counter` below leaves no surviving
// symbol use at all -- so the gate reads the whole-program address-taken
// pre-scan, and this pin holds it to that.
extern int host_counter;

int *counter_loc(void) { return &host_counter; }

int bump(void) {
  host_counter = host_counter + 1;
  return host_counter;
}
// GLOBAL-ADDR: error: unsupported: extern global variable 'host_counter' is referenced but not defined in any translation unit

//--- global-agg.c
// A STRUCT extern global no longer rejects on its type: FR-79 admitted the
// const struct (getter-only) and FR-81 the non-const struct (whole-value
// getter/setter pair -- sequentially, staged get/modify/set through a Copy
// struct is exact). The frontier is now decided on the USE SHAPE: whole-value
// loads and stores qualify, while addresses into the global (and arrays,
// whose element access needs a place) keep the rejection -- see
// multi-tu-external-requirement-nonconst-struct-negative.c. This arm pins
// the flip of FR-70's recorded aggregate refusal.
struct cfg {
  int a;
  int b;
};
extern struct cfg host_cfg;

int first(void) {
  struct cfg snapshot = host_cfg;
  return snapshot.a;
}
// GLOBAL-AGG: emitrust.global @host_cfg {emitrust.external_requirement} : !emitrust.struct<"cfg">

//--- variadic.c
// An undefined VARIADIC external never reaches the requirement decision at
// all: a body-less variadic declaration is not imported, so its call site
// carries the rejection instead -- earlier, and located on the C construct
// rather than on a whole-program fact.
int host_logf(const char *fmt, ...);

int log_one(int v) { return host_logf("v=%d", v); }
// VARIADIC: error: unsupported: call to a variadic function
