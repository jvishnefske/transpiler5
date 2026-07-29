// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/compile_commands.json.in > %t/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t -o - | FileCheck %s

// FR-45 argument filtering: a real `compile_commands.json` records the
// arguments the project's own DRIVER ran with, several of which must never
// reach the frontend. `a.c`'s entry carries the separated spellings
// (`-c -o a.o -MD -MF a.d -MT a.o -MQ a.o`) and `b.c`'s the joined ones
// (`-ob.o -MFb.d -MTb.o`) plus the standalone `-M`/`-MMD`; the
// operand-taking ones are the dangerous shape, since a bare `a.d` left in
// the argument list is read as a SECOND input file. Both entries also keep
// the compiler `argv[0]` that is dropped and replaced by a neutral driver
// name.
//
// `ClangTool` installs default `ArgumentsAdjuster`s that today strip an
// overlapping set, so this test is a contract pin rather than a crash
// reproducer: it asserts that a database entry of the shape every build
// system emits imports correctly, whichever layer does the stripping, and
// it fails if OUR filter ever starts removing more than it should.
//
// Flags that must survive the filter ride along in the same entries: the
// `-D` macros and the `-I` are load-bearing for the checked output, so an
// over-stripping filter fails just as loudly as an under-stripping one.

//--- compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/a.c",
    "command": "/usr/bin/cc -DSCALE=7 -Iinc -c -o a.o -MD -MF a.d -MT a.o -MQ a.o a.c"
  },
  {
    "directory": "@DIR@",
    "file": "@DIR@/b.c",
    "command": "/usr/bin/cc -DOFFSET=5 -Iinc -M -MMD -ob.o -MFb.d -MTb.o -c b.c"
  }
]

//--- inc/limits_local.h
#ifndef LIMITS_LOCAL_H
#define LIMITS_LOCAL_H
#define BUMP 1
#endif

//--- a.c
#include "limits_local.h"

int scaled(int x);
int offset(int x);

int main(void) { return scaled(2) + offset(3); }

int scaled(int x) { return x * SCALE + BUMP; }

//--- b.c
#include "limits_local.h"

int offset(int x) { return x + OFFSET + BUMP; }

// CHECK-DAG: func.func @scaled
// CHECK-DAG: func.func @offset
// CHECK-DAG: func.func @c_main
// The surviving -D macros are visible as the constants they expand to.
// CHECK-DAG: arith.constant 7
// CHECK-DAG: arith.constant 5
