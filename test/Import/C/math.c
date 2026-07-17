// RUN: split-file %s %t
// RUN: emitrust-import-c %t/sin-call.c | FileCheck %s --check-prefix=SIN
// RUN: not emitrust-import-c %t/tgamma-call.c 2>&1 | FileCheck %s --check-prefix=TGAMMA
// RUN: emitrust-import-c %t/user-sin.c | FileCheck %s --check-prefix=USER

// C99-48: the hosted <math.h> surface. A definition-less call to `sin`
// with its standard double(double) prototype lowers to
// `emitrust.call_opaque "f64::sin"`; every other math function keeps the
// system-header rejection with a located diagnostic, and a user-defined
// `sin` stays an ordinary call.

//--- sin-call.c
#include <math.h>

int main(void) {
  double x = 1.5;
  double y = sin(x);
  double z = sin(2);
  if (y < z) {
    return 1;
  }
  return 0;
}

// The math.h declaration itself is never imported; each call site becomes
// an opaque call to the f64 method. The int argument of the second call
// goes through the usual conversion to double first.
// SIN-NOT: func.func private @sin
// SIN-LABEL: func.func @c_main() -> i32
// SIN: %{{.*}} = emitrust.call_opaque "f64::sin"(%{{.*}}) : (f64) -> f64
// SIN: %[[W:.*]] = arith.sitofp %{{.*}} : i32 to f64
// SIN: %{{.*}} = emitrust.call_opaque "f64::sin"(%[[W]]) : (f64) -> f64
// SIN-NOT: func.func private @sin

//--- tgamma-call.c
#include <math.h>

int main(void) {
  double y = tgamma(4.0);
  if (y > 0.0) {
    return 1;
  }
  return 0;
}

// TGAMMA: tgamma-call.c:4:14: error: unsupported: call to 'tgamma' declared in a system header; not part of the supported C subset

//--- user-sin.c
double sin(double x) {
  return x + 1.0;
}

int main(void) {
  double y = sin(1.0);
  if (y > 0.0) {
    return 1;
  }
  return 0;
}

// A project-defined sin is an ordinary imported function; the hosted
// mapping only intercepts definition-less declarations.
// USER: func.func @sin(%{{.*}}: f64) -> f64
// USER-LABEL: func.func @c_main() -> i32
// USER: call @sin(%{{.*}}) : (f64) -> f64
// USER-NOT: f64::sin
