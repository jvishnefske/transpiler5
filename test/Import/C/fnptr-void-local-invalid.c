// CTS-F (00210) negative space: the local void* fn-holder admission
// covers ONLY a never-reassigned local initialized from one known
// function whose every value use is a cast-call spelling EXACTLY the
// target's signature in callee position. A reassigned holder, a holder
// cast to a mismatched signature, and a holder that escapes as a call
// argument all keep the existing located rejection (existing wording,
// reused; the location may move between the init and the offending use,
// so only file:line:col wildcards are pinned).
// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/reassigned.c 2>&1 | FileCheck %s --check-prefix=REASSIGNED
// RUN: not emitrust-import-c %t/mismatch.c 2>&1 | FileCheck %s --check-prefix=MISMATCH
// RUN: not emitrust-import-c %t/escapes.c 2>&1 | FileCheck %s --check-prefix=ESCAPES

//--- reassigned.c
// Two different targets flow into the holder: not a constant fn-ptr.
int f(void) { return 1; }
int g(void) { return 2; }
int main(void) {
  void *fp = &f;
  fp = &g;
  return ((int (*)(void))fp)();
}
// REASSIGNED: reassigned.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- mismatch.c
// The cast type does not match f's signature: calling through it would
// be UB, so the holder never admits.
int f(void) { return 1; }
int main(void) {
  void *fp = &f;
  return ((int (*)(int))fp)(3);
}
// MISMATCH: mismatch.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value

//--- escapes.c
// The holder is passed to a function (through a variadic prototype — a
// declared `void *` parameter is itself already rejected earlier, which
// would shadow the holder analysis): a non-cast-call value use, so the
// holder never admits.
int f(void) { return 1; }
void sink(int n, ...);
int main(void) {
  void *fp = &f;
  sink(1, fp);
  return ((int (*)(void))fp)();
}
// ESCAPES: escapes.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: pointer assigned a non-address value
