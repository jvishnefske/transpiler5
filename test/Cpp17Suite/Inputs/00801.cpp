// Cpp17Suite 00801: corpus-only copy-elision sensor -- NO implementation
// task. The counting copy constructor is outside the subset, so this stays
// UNSUPPORTED today; if the transpiler ever silently accepts it and
// materializes an extra copy, the printed count diverges from the native
// leg and the ratchet flags MISCOMPILE. C++17 GUARANTEES elision for the
// prvalue factory return, so copies=0 is the only correct output (no
// optional-NRVO nondeterminism is observed).
extern "C" int printf(const char *, ...);

int copies = 0;

struct Tracer {
  int value;
  Tracer(int v) : value(v) {}
  Tracer(const Tracer &other) : value(other.value) { copies += 1; }
};

Tracer make_tracer(int v) { return Tracer(v); }

int main() {
  Tracer t = make_tracer(41);
  printf("value=%d copies=%d\n", t.value, copies);
  return 0;
}
