// RUN: split-file %s %t
// RUN: emitrust-import-c %t/argc-only.c | FileCheck %s --check-prefix=ARGC
// RUN: not emitrust-import-c %t/argv-used.c 2>&1 | FileCheck %s --check-prefix=ARGVUSED
// RUN: not emitrust-import-c %t/one-param.c 2>&1 | FileCheck %s --check-prefix=ONEPARAM

// C `main`'s standard two-parameter form (C99 5.1.2.2.1): `argc` imports
// as a plain i32 parameter of `c_main` (the crate wrapper passes the
// process argument count), and `argv` — whose `char **` shape has no safe
// decomposition — is dropped from the imported signature. A body that
// reads `argv` and any non-standard parameter list keep located
// rejections.

//--- argc-only.c
int main(int argc, char **argv) {
  return argc > 1;
}
// ARGC-LABEL: func.func @c_main
// ARGC-SAME: (%{{.*}}: i32) -> i32
// ARGC-NOT: char
// ARGC-NOT: mut_ref

//--- argv-used.c
int main(int argc, char **argv) {
  return argv[0] != 0;
}
// ARGVUSED: argv-used.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: use of main's argv parameter (command-line argument values are not modeled)

//--- one-param.c
int main(int argc) {
  return argc;
}
// ONEPARAM: one-param.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: main must take zero or two (int, char **) parameters
