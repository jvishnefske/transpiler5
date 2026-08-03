// Library TU for link-merge-rejections.c: one good function the main TU
// calls, and one function the shim's recovering import rejects (volatile)
// and stubs — its ledger entry must ride the artifact to the link step.

int lib_good(int x) { return x + 1; }

int lib_rejected(int n) {
  volatile int v = n; // rejected under the recovering import; stubbed
  return v;
}
