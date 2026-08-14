// FR-52 boundaries: the shapes that keep rejecting even under
// --externals-trait, each because the trait cannot express them faithfully.
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/addr-taken.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR %s
// RUN: not emitrust-import-c --externals-trait %t/global-addr.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=GLOBAL-ADDR %s
// RUN: not emitrust-import-c --externals-trait %t/global-agg.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=GLOBAL-AGG %s
// RUN: not emitrust-import-c --externals-trait %t/variadic.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=VARIADIC %s

//--- empty.c
int unrelated(int v) { return v; }

//--- addr-taken.c
// An address-taken function is emitted as the opaque constant `Some(host_op)`,
// which spells the item out directly. The trait's type parameter cannot reach
// inside that text, so routing the CALL through `E::host_op` would leave the
// fn-pointer reference dangling. The whole symbol therefore keeps rejecting.
int host_op(int v);

int apply(int (*f)(int), int v) { return f(v); }

int run(int v) { return apply(host_op, v) + host_op(v); }
// ADDR: error: unsupported: function 'host_op' is referenced but not defined in any translation unit

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
// An AGGREGATE extern global keeps rejecting too, even though its accesses
// lower to whole-value load/store at the IR level: the trait passes values
// by copy, and C code holding `extern struct` storage expects places (member
// projections, element addresses) the pair cannot faithfully model. The
// frontier is decided on the TYPE, not on the IR use shape.
struct cfg {
  int a;
  int b;
};
extern struct cfg host_cfg;

int first(void) {
  struct cfg snapshot = host_cfg;
  return snapshot.a;
}
// GLOBAL-AGG: error: unsupported: extern global variable 'host_cfg' is referenced but not defined in any translation unit

//--- variadic.c
// An undefined VARIADIC external never reaches the requirement decision at
// all: a body-less variadic declaration is not imported, so its call site
// carries the rejection instead -- earlier, and located on the C construct
// rather than on a whole-program fact.
int host_logf(const char *fmt, ...);

int log_one(int v) { return host_logf("v=%d", v); }
// VARIADIC: error: unsupported: call to a variadic function
