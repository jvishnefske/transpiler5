// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/compile_commands.json.in > %t/compile_commands.json
// RUN: emitrust-cc --emit=import --compdb %t -o %t/from-db.mlir
// RUN: emitrust-cc --emit=import %t/a.c %t/b.c -o %t/explicit.mlir
// RUN: diff %t/explicit.mlir %t/from-db.mlir
// RUN: FileCheck %s < %t/from-db.mlir
// RUN: emitrust-cc --emit=import --compdb %t/compile_commands.json -o - | FileCheck %s
// RUN: emitrust-cc --emit=import --compdb %t %t/b.c -o - | FileCheck %s --check-prefix=SUBSET

// FR-45: a two-translation-unit project imported entirely from a
// `compile_commands.json`, with NO positional sources on the command line
// — the database's file list is the project. The `diff` against the
// historical spelling (both sources named explicitly, no database) is the
// real assertion: the database path must produce a byte-identical module,
// which also pins the deterministic translation-unit order the importer
// derives from the database (sorted by path, so `a.c` is TU0 and `b.c` is
// TU1 regardless of the order clang's StringMap hands them back).
//
// The last two RUN lines pin the remaining surface: --compdb accepts the
// JSON file itself as well as the directory containing it, and a
// positional source given ALONGSIDE --compdb narrows the project to that
// one file (its flags still coming from the database) instead of adding
// to it.
//
// Each entry deliberately records its input RELATIVE to its `directory`
// field while the `file` key is absolute — the shape CMake emits, and the
// one a naive consumer resolving paths against its own working directory
// gets wrong.

//--- compile_commands.json.in
[
  {
    "directory": "@DIR@",
    "file": "@DIR@/a.c",
    "command": "cc -c a.c -o a.o"
  },
  {
    "directory": "@DIR@",
    "file": "@DIR@/b.c",
    "command": "cc -c b.c -o b.o"
  }
]

//--- a.c
int helper(int x);

int main(void) { return helper(2); }

//--- b.c
int helper(int x) { return x + 1; }

// Both translation units are present in the one merged module, with the
// cross-TU call resolved against the definition in the other file.
// CHECK-LABEL: func.func @c_main
// CHECK: call @helper
// CHECK-LABEL: func.func @helper

// Naming b.c positionally imports b.c and nothing else: a.c's `main` is
// not pulled in just because the database lists it.
// SUBSET-NOT: func.func @c_main
// SUBSET: func.func @helper
// SUBSET-NOT: func.func @c_main
