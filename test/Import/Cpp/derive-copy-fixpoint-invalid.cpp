// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

// FR-124: the failure DIRECTION for by-value use of a fixpoint-non-Copy
// struct. D over the `virtual ~B() = default` base carries no has_drop of
// its own (the inherited walk counts only user-PROVIDED dtors), so the
// W2.17 "class with a destructor passed by value" fence does not name it;
// the by-value call still dies TODAY as a LOCATED CXXBindTemporaryExpr
// rejection -- the frontline. Behind it, rustc E0382 (use of moved value)
// is the second-line backstop once the derive no longer says Copy: loud
// either way, never a silent bitwise copy of a polymorphic C++ object.
// This pin exists so that any wave admitting the shape must consciously
// move it, with the byte-diff oracle in the loop -- not inherit a fixpoint
// hole. Wording probed from the built importer, not guessed.

struct B {
  int tag;
  virtual ~B() = default;
};

struct D : B {
  double weight;
};

int take(D d) { return d.tag; }

int main(int argc, char **) {
  D d;
  d.tag = argc;
  d.weight = 1.5;
  // CHECK: derive-copy-fixpoint-invalid.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported expression: CXXBindTemporaryExpr
  int a = take(d);
  int b = take(d);
  return a + b;
}
