// Report-test TU exercising the kernel-flavored constructs: a volatile
// local (tag `other`) and inline asm (tag `unsupported-stmt:GCCAsmStmt`).

int vread(int n) {
  volatile int v = n;
  return v;
}

int asmish(int x) {
  __asm__ volatile("" : "+r"(x));
  return x;
}
