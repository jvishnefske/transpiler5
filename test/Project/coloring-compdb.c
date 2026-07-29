// RUN: split-file %s %t
// RUN: sed -e "s|@DIR@|%t|g" %t/compile_commands.json.in > %t/compile_commands.json
// RUN: emitrust-cc --emit=coloring --compdb %t -o %t/from-db.txt
// RUN: emitrust-cc --emit=coloring %t/a.c %t/b.c -o %t/explicit.txt
// RUN: diff %t/explicit.txt %t/from-db.txt
// RUN: FileCheck %s < %t/from-db.txt
// RUN: emitrust-cc --emit=coloring --compdb %t/compile_commands.json -o - | FileCheck %s
// RUN: emitrust-cc --emit=coloring --compdb %t %t/b.c -o - | FileCheck %s --check-prefix=SUBSET

// FR-41 composed with FR-45: `--emit=coloring` takes its project from a
// `compile_commands.json` exactly as `--emit=item-graph` does, and for the
// same reason — the coloring must describe the project the importer would
// actually see, flags, language, and all, or its Green subset would be a
// claim about a program nobody compiles. It goes through the identical shell
// (`buildProjectASTs`), so the whole surface comes along: the database
// directory or the JSON file itself, the database supplying the entire source
// list when no positional input is given, and a positional input narrowing the
// project to a subset while still taking that file's recorded flags.
//
// The `diff` against the historical spelling is the real assertion: taking the
// same two files through the database must produce byte-identical output,
// which also pins that the database's deterministic translation-unit order
// (sorted by path) reaches the coloring.

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
int helper(int x) {
  __asm__("");
  return x + 1;
}

// The cross-translation-unit poison edge: `helper` is Red for a body construct
// in b.c, and `c_main` in a.c is demoted to Yellow through it.
// CHECK:      item c_main kind=function color=yellow reason=stub-callee via=helper edge=Calls chain=c_main->helper construct=inline-asm
// CHECK-NEXT: item helper kind=function color=red reason=inadmissible construct=inline-asm
// CHECK-NEXT: tally green=0 yellow=1 red=1

// Naming b.c positionally colors b.c and nothing else: a.c's `main` is not
// pulled in just because the database lists it, so nothing is left to be
// Yellow.
// SUBSET-NOT: item c_main
// SUBSET:     item helper kind=function color=red reason=inadmissible construct=inline-asm
// SUBSET-NEXT: tally green=0 yellow=0 red=1
