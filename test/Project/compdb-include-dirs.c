// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/compile_commands.json.in > %t/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t -o - | FileCheck %s
// RUN: emitrust-cc --emit=import --compdb %t %t/a.c -o - | FileCheck %s
// RUN: not emitrust-cc --emit=import %t/a.c -o - 2>&1 | FileCheck %s --check-prefix=NODB

// FR-45: the include search path exists ONLY inside the database entry.
// `a.c` includes "helper.h", which lives in a sibling `inc/` directory
// reachable through nothing but the entry's `-I` — and the entry spells
// that `-I` RELATIVE to its own `directory` field, so the flag is proof of
// two things at once: the flags really come from the database, and they
// are resolved against the entry's directory rather than the process's
// working directory.
//
// The second run pins the other selection mode: naming the source
// positionally ALONGSIDE --compdb imports only that file, still with the
// database's flags (the same -I it never spells on the command line).
//
// The NODB run is the control: the identical source with no database and
// no hand-passed -I must fail to find the header, which is what makes the
// first two runs' success meaningful.

//--- compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/a.c",
    "command": "cc -Iinc -c a.c -o a.o"
  }
]

//--- inc/helper.h
#ifndef HELPER_H
#define HELPER_H
static int scale(int x) { return x * 3; }
#endif

//--- a.c
#include "helper.h"

int main(void) { return scale(2); }

// CHECK: func.func @c_main
// CHECK: call @{{(tu0_)?}}scale

// NODB: error: 'helper.h' file not found
