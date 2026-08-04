// RUN: split-file %s %t
// RUN: emitrust-import-c %t/admitted.c | FileCheck %s --check-prefix=ADMIT
// RUN: emitrust-import-c %t/argc-only.c | FileCheck %s --check-prefix=ARGC

// C99-43 C3: C `main`'s `char **argv` imports as the opaque
// `!emitrust.argv_table` parameter (rendered `&[Vec<i8>]`) when EVERY use
// fits the admitted read grammar — a whole-value `argv[i]` fed to a direct
// `printf` `%s`, and `argv[i][j]` bytes consumed as VALUES (a `%c` hole, a
// NUL-scan comparison, a `%d`). The `%s`/`%c` holes BYPASS the Latin-1
// Display funnels: the pending format segment flushes as its own `print!`
// and the raw bytes go through the `__emitrust_cstr_out`/`__emitrust_byte_out`
// on-demand helpers. An argc-only `main` still drops argv at arity 1,
// byte-identical to before C3. This pins the admitted signature seat, the
// `emitrust.argv_arg` borrow, and the printf bypass split.

//--- admitted.c
int printf(const char *, ...);

int main(int argc, char **argv) {
  printf("argc=%d\n", argc);
  for (int i = 1; i < argc; i++)
    printf("%s\n", argv[i]);
  if (argc > 1) {
    printf("first=%c\n", argv[1][0]);
    int n = 0;
    while (argv[1][n])
      n++;
    printf("len=%d\n", n);
  }
  return 0;
}
// The admitted signature carries the argv table alongside argc.
// ADMIT-LABEL: func.func @c_main
// ADMIT-SAME: (%{{.*}}: i32, %{{.*}}: !emitrust.argv_table) -> i32
// A whole `argv[i]` %s hole borrows the argument slice and prints it raw,
// with the trailing "\n" flushed as its own segment.
// ADMIT: emitrust.argv_arg %{{.*}}[%{{.*}}] : (!emitrust.argv_table, i32) -> !emitrust.ref<!emitrust.slice<i8>>
// ADMIT: emitrust.call_opaque "__emitrust_cstr_out"(%{{.*}}) : (!emitrust.ref<!emitrust.slice<i8>>) -> ()
// A %c of an argv byte flushes "first=" then writes the raw byte value.
// ADMIT: emitrust.call_opaque "print!"() {args = ["first="]}
// ADMIT: emitrust.argv_arg
// ADMIT: emitrust.deref
// ADMIT: emitrust.subscript
// ADMIT: emitrust.call_opaque "__emitrust_byte_out"(%{{.*}}) : (i8) -> ()
// The raw stdout helpers are emitted once at module end.
// ADMIT: emitrust.verbatim "fn __emitrust_cstr_out
// ADMIT: emitrust.verbatim "fn __emitrust_byte_out

//--- argc-only.c
int main(int argc, char **argv) {
  return argc > 1;
}
// An unused argv keeps the dropped-parameter arity-1 signature: no table.
// ARGC-LABEL: func.func @c_main
// ARGC-SAME: (%{{.*}}: i32) -> i32
// ARGC-NOT: argv_table
