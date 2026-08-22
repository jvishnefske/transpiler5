// REQUIRES: cargo
// FR-120 item 3: UPCAST pointer bindings (`Base *p = &d;`), byte-diffed
// against `clang++ -std=c++17`. Under W2.18's admission guard (single,
// public, non-virtual, non-polymorphic base chain) the hop path from the
// bound derived record to any viewed base record is UNIQUE and
// recomputable from the two types alone, so the pointer binds the WHOLE
// derived object and every use site re-derives the `member ["base"]`
// hops (`reconcileUpcastPlace`) -- no new binding kind, no new dialect
// op, pointer fully erased.
//
// THE SHADOWED-FIELD LEGS (`sf=`/`sw=`) ARE THE SOUNDNESS PIN for a
// missing reconcile. `emitPointerPlace` has no pointee type check and
// `emitrust.member` no field-existence check, so a naive peel that skips
// the hop projection hands `r->x` the DERIVED place: with `Derived`
// shadowing `Base::x` that crate COMPILES CLEAN and prints the wrong
// subobject (measured in the FR-120 spike) -- rustc cannot catch it, only
// this byte-diff can. The plain E0609 variant (no shadowing) under-pins
// the bug, which is why `Derived` here declares its own `x`.
//
// The rest of the matrix: inherited field READ and WRITE through the
// base pointer (`p->a`, `p->a += 3` landing on `d`), base method and
// MUTATING base method, the two-level A<-B<-C chain (one cast node, two
// recomputed hops), inherited access through a DERIVED-typed pointer
// (`pc->adda`/`pc->a`, the inheritance-invalid.cpp DERIVEDPTR pin moved
// forward -- no upcast at the binding, hops projected at the member),
// non-virtual name hiding (`p->tag()` binds Base::tag
// while `d.tag()` binds Derived::tag -- static binding is the admitted
// semantics), and a DROPPY base chain (`RB`/`RD`, the pin moved forward
// from inheritance-drop-invalid.cpp's UPCAST arm): `~RB` must run
// exactly once, at `rd`'s scope exit, with the pointer's mutation
// visible in the destructor's output.
//
// Every value derives from argc and objects live at function scope, so
// constant folding cannot hide a miscompile.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_upcast_pointer > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

struct A {
  int a;
  int geta() const { return a; }
  void adda(int d) { a += d; }
  int tag() const { return 1; }
};
struct B : A { int b; };
struct C : B { int c; };

// Non-virtual name hiding AND a shadowed data member: `Base *` accesses
// must bind Base's members, direct `Derived` accesses Derived's.
struct Base {
  int x;
  int tag() const { return 1; }
};
struct Derived : Base {
  int x; // shadows Base::x -- the missing-reconcile miscompile channel
  int tag() const { return 2; }
};

// A droppy base chain: the upcast binding must not disturb the
// single-run drop at the derived object's scope exit.
struct RB {
  int x;
  RB(int v) : x(v) {}
  int get() const { return x; }
  void bump(int d) { x += d; }
  ~RB() { printf("~RB %d\n", x); }
};
struct RD : RB {
  int y;
  RD(int v) : RB(v), y(v + 1) {}
};

int main(int argc, char **) {
  B d;
  d.a = argc;
  d.b = argc + 1;
  A *p = &d;
  p->adda(2); // mutating base method through Base*
  printf("f=%d m=%d d=%d\n", p->a, p->geta(), d.b);
  p->a += 3; // inherited field write through Base*
  printf("w=%d\n", d.a);
  C e;
  e.a = argc;
  e.b = 2;
  e.c = 3;
  A *q = &e; // two-level chain: one cast node, two recomputed hops
  q->adda(4);
  printf("2l=%d %d\n", q->a, q->geta());
  C *pc = &e; // DERIVED-typed pointer, inherited access (DERIVEDPTR flip)
  pc->adda(8);
  printf("dp=%d %d %d\n", pc->a, pc->geta(), pc->c);
  printf("t=%d %d\n", p->tag(), q->tag());
  Derived h;
  h.Base::x = argc;
  h.x = argc + 10;
  Base *r = &h;
  printf("h=%d %d\n", r->tag(), h.tag()); // name hiding: 1 then 2
  printf("sf=%d %d\n", r->x, h.x);        // shadowed field: base then derived
  r->x += 5;                              // write must land on Base::x
  printf("sw=%d %d\n", h.Base::x, h.x);
  RD rd(argc);
  RB *rp = &rd;
  rp->bump(2);
  printf("r=%d\n", rp->get());
  return 0; // ~RB runs here, exactly once, with the +2 visible
}
