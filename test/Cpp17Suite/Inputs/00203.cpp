// Cpp17Suite 00203: task-002 target -- condition-declaration form
// (`if (int x = f())`, legal since C++98 but sharing task-002's
// condition-scope machinery): the declared variable IS the condition and
// is in scope in both branches.
extern "C" int printf(const char *, ...);

int probe(int v) { return v - 4; }

int check(int v) {
  if (int r = probe(v)) {
    return r * 2;
  } else {
    return -1;
  }
}

int main() {
  printf("%d %d\n", check(10), check(4));
  return 0;
}
