// FR-52 boundaries: the shapes that keep rejecting even under
// --externals-trait, each because the trait cannot express them faithfully.
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/addr-taken.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ADDR %s
// RUN: not emitrust-import-c --externals-trait %t/global.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=GLOBAL %s
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

//--- global.c
// An undefined external OBJECT rejects unconditionally. A trait can carry an
// associated const, which is a VALUE; a C `extern int` denotes mutable
// STORAGE with an address, and there is no associated item that yields a
// place a translated assignment could write to.
extern int host_counter;

int bump(void) {
  host_counter = host_counter + 1;
  return host_counter;
}
// GLOBAL: error: unsupported: extern global variable 'host_counter' is referenced but not defined in any translation unit

//--- variadic.c
// An undefined VARIADIC external never reaches the requirement decision at
// all: a body-less variadic declaration is not imported, so its call site
// carries the rejection instead -- earlier, and located on the C construct
// rather than on a whole-program fact.
int host_logf(const char *fmt, ...);

int log_one(int v) { return host_logf("v=%d", v); }
// VARIADIC: error: unsupported: call to a variadic function
