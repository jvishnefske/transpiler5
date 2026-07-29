// REQUIRES: cargo
// FR-47: intra-class method calls through the IMPLICIT `this` receiver,
// end to end. Where cpp-method-chains.cpp threads values BETWEEN instances
// (every call site has an explicit receiver), this one never writes a
// receiver at all inside the class: every call in a method body is a bare
// `m();`, the shape that rejected outright before FR-47 with
// "unsupported assignable expression: CXXThisExpr".
//
// Exercises, all in one program so a wrong receiver borrow or a
// mis-sequenced call surfaces as a stdout diff rather than as a build
// error: a mutating method calling a mutating sibling twice in one body
// (`Lexer::feed` -> `finish`, the real `tokenizer` corpus shape); a
// `const` method calling a `const` sibling (shared borrow on both sides);
// a mutating method calling a `const` sibling (mutable receiver argument,
// shared borrow); a constructor calling a method declared AFTER it; a
// method calling a sibling declared after it; direct recursion through
// `this`; a three-deep implicit-`this` call chain; and the explicit
// `this->m()` and `(*this).m()` spellings interleaved with the implicit
// one, which must all behave identically. Every printed value is
// data-dependent on the mutation order, so a dropped or duplicated call
// changes the output.
//
// The native leg is clang++ -std=c++17 (the source is C++); printf is
// declared `extern "C"` so the native build links libc's printf rather
// than mangling a C++-linkage lookup for it, matching cpp-basics.cpp.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cpp_method_chain > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern "C" int printf(const char *, ...);

// A character-at-a-time run classifier: the distilled shape of the
// `tokenizer` RealWorld project, whose `feed` calls a bare `finish()`.
// `feed` is declared before `finish`, so the sibling call also depends on
// FR-47's signature prepass (a C++ member body is a complete-class
// context; the importer used to resolve only already-imported callees).
class Runs {
public:
  // Constructor calling a method declared later: the ordinary way such a
  // class is written, and unsupported before FR-47.
  Runs() : state_(0), run_(0), words_(0), longest_(0), banked_(0) { reset(); }

  void feed(int kind) {
    if (kind == state_ && kind != 0) {
      run_ = run_ + 1;
      return;
    }
    finish(); // implicit `this`, mutating -> mutating
    state_ = kind;
    run_ = kind == 0 ? 0 : 1;
  }

  // Two implicit-`this` calls in ONE body, plus the two explicit
  // spellings, so all three forms are compared against the same native
  // build in the same run.
  void feed_pair(int a, int b) {
    feed(a);
    this->feed(b);
  }

  void feed_star(int a) { (*this).feed(a); }

  void finish() {
    if (state_ != 0) {
      words_ = words_ + 1;
      if (run_ > longest_)
        longest_ = run_;
    }
    state_ = 0;
    run_ = 0;
  }

  void reset() {
    state_ = 0;
    run_ = 0;
    words_ = 0;
    longest_ = 0;
  }

  // `const` -> `const`: both receivers are shared borrows.
  int words() const { return words_; }
  int longest() const { return longest_; }
  int score() const { return words() * 10 + longest(); }

  // Mutating -> `const`: mutable receiver argument, shared borrow of it.
  void bank() { banked_ = banked_ + score(); }
  int banked() const { return banked_; }

  // Direct recursion through the implicit receiver.
  int countdown(int n) {
    if (n <= 0)
      return banked_;
    banked_ = banked_ + n;
    return countdown(n - 1);
  }

private:
  int state_;
  int run_;
  int words_;
  int longest_;
  int banked_;
};

// A three-deep implicit-`this` chain, each level declared AFTER its
// caller, so every edge needs the signature prepass.
class Chain {
public:
  Chain() : n_(1) {}
  int top() { return mid() + 1000; }
  int mid() { return bottom() + 100; }
  int bottom() {
    n_ = n_ + 1;
    return n_;
  }
  int n() const { return n_; }

private:
  int n_;
};

int main(void) {
  Runs r;
  // A word run of length 3, then punctuation, then a run of length 2.
  r.feed_pair(1, 1);
  r.feed(1);
  r.feed_star(0);
  r.feed_pair(2, 2);
  r.finish();
  printf("words=%d longest=%d score=%d\n", r.words(), r.longest(), r.score());

  r.bank();
  printf("banked=%d\n", r.banked());
  printf("countdown=%d\n", r.countdown(4));

  r.reset();
  printf("after reset: words=%d longest=%d banked=%d\n", r.words(),
         r.longest(), r.banked());

  Chain c;
  printf("chain=%d n=%d\n", c.top(), c.n());
  printf("chain again=%d n=%d\n", c.top(), c.n());
  return 0;
}
