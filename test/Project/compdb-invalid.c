// RUN: split-file %s %t
// RUN: not emitrust-cc --emit=import --compdb %t/absent/compile_commands.json %t/a.c -o - 2>&1 | FileCheck %s --check-prefix=MISSING
// RUN: not emitrust-cc --emit=import --compdb %t/absent %t/a.c -o - 2>&1 | FileCheck %s --check-prefix=MISSINGDIR
// RUN: not emitrust-cc --emit=import --compdb %t/malformed -o - 2>&1 | FileCheck %s --check-prefix=MALFORMEDDIR
// RUN: not emitrust-cc --emit=import --compdb %t/malformed/compile_commands.json -o - 2>&1 | FileCheck %s --check-prefix=MALFORMEDFILE
// RUN: not emitrust-cc --emit=import --compdb %t/notproject -o - 2>&1 | FileCheck %s --check-prefix=NOTPROJECT

// FR-45 negative surface: a database that cannot be loaded is a located
// diagnostic naming the offending path and carrying clang's own
// explanation, never a crash and never a silent fall back to the
// extension guess (which would import the sources with the WRONG flags
// and produce a plausible-looking but wrong module).
//
// Four ways to get it wrong are pinned: a database file that does not
// exist, a directory that does not exist, a file whose contents are not a
// compilation database, and a directory containing no
// `compile_commands.json` at all.

//--- a.c
int main(void) { return 0; }

//--- malformed/compile_commands.json
{ this is not a compilation database

//--- notproject/README.txt
This directory deliberately contains no compile_commands.json.

// Every message is one line, prefixed by the offending path as a
// location, and ends in clang's own explanation (the directory form
// aggregates one reason per registered database plugin).
// MISSING: absent/compile_commands.json:1:1: error: cannot load compilation database: Error while opening JSON database: No such file or directory
// MISSINGDIR: absent:1:1: error: cannot load compilation database: {{.*}}No such file or directory
// MALFORMEDDIR: malformed:1:1: error: cannot load compilation database: {{.*}}json-compilation-database: Expected array.
// MALFORMEDFILE: malformed/compile_commands.json:1:1: error: cannot load compilation database: Expected array.
// NOTPROJECT: notproject:1:1: error: cannot load compilation database: {{.*}}No such file or directory
