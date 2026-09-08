// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/inc.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %t/star.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %t/store.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %t/addr.c 2>&1 | FileCheck %s
// RUN: not emitrust-import-c %t/width.c 2>&1 | FileCheck %s

// C99-43 C3: any use of `main`'s `argv` outside the admitted read grammar
// leaves the parameter unadmitted, so the historical located rejection
// stands at the signature — its wording and ledger tag byte-identical to
// before C3. This pins the loud-failure direction and the exact wording
// twin (RejectionLedger.cpp `{"use of main's argv", "argv"}`) for the
// non-admitted shapes: pointer arithmetic (`argv++`), a bare `*argv`,
// storing `argv[i]` to a local, taking `&argv[i]`, and a `%s` with an
// explicit field width.
//
// The width case's ORIGINAL reason -- "the raw `*_out` helper cannot pad" --
// stopped being true with FR-193 item 2, which added the padding raw helper
// `__emitrust_cstr_pad_out` for char REGIONS. The argv admission grammar
// (`admittedPrintfStringArg`) was deliberately NOT widened with it: widening
// it changes whether `main` gets an argv table at all, which is a planning
// decision with a corpus-ratchet blast radius, and the current behaviour is a
// LOCATED rejection rather than a silent width drop. So this stays pinned --
// as a capability gap now, not as a representation limit.

// CHECK: error: unsupported: use of main's argv parameter (command-line argument values are not modeled)

//--- inc.c
int printf(const char *, ...);
int main(int argc, char **argv) {
  argv++;
  return **argv;
}

//--- star.c
int printf(const char *, ...);
int main(int argc, char **argv) {
  char *p = *argv;
  return *p;
}

//--- store.c
int printf(const char *, ...);
int main(int argc, char **argv) {
  char *p = argv[0];
  return *p;
}

//--- addr.c
int printf(const char *, ...);
int main(int argc, char **argv) {
  char **p = &argv[0];
  return **p;
}

//--- width.c
int printf(const char *, ...);
int main(int argc, char **argv) {
  printf("%10s", argv[0]);
  return 0;
}
