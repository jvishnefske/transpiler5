// W2.24: the exceptions reclassification in the admissibility probe.
//
// Under Result threading a function whose body THROWS does not keep its
// original signature — the importer rewrites it to return the synthesized
// carrier enum (`ThrowsI32`), and the rewrite is viral through the
// transitive caller set. So a THROW is SIGNATURE-level: no stub with the
// original signature can be written (a stub cannot invent the carrier),
// and the probe's old body-level comment ("the caller stays Yellow") was
// measured FALSE for this tag. A TRY/CATCH-only body stays BODY-level: a
// function that handles everything locally keeps its declared signature,
// so it is still stubbable and its callers only go Yellow — moving it to
// signature-level would be a false Red, the probe's forbidden direction.
//
// The split is pinned in both directions below: `boom` (throw) drags its
// direct caller Red, exactly one hop, while `shields` (try only, no
// throw) leaves its caller Yellow.
// RUN: emitrust-cc --emit=coloring %s -o - | FileCheck %s

/// Red, signature-level: the body throws, so the signature is rewritten.
int boom(int x) {
  if (x)
    throw 7;
  return x;
}

/// Red: its callee has no stub (signature-level exceptions).
int calls_boom(int x) { return boom(x); }

/// Yellow: `calls_boom` is Red but its OWN signature is clean, so it is
/// stubbable — Red travels arbitrarily far, unstubbability exactly one hop.
int calls_calls_boom(int x) { return calls_boom(x); }

/// Red, but BODY-level: try/catch with no throw handles everything
/// locally; the declared signature survives, so a stub can be written.
int shields(int x) {
  int r = 0;
  try {
    r = x + 1;
  } catch (int e) {
    r = e;
  }
  return r;
}

/// Yellow: its callee is Red but stubbable.
int calls_shields(int x) { return shields(x); }

int main() { return calls_calls_boom(1) + calls_shields(2); }

// `boom` is signature-level Red, so `calls_boom` goes Red (red-callee,
// the W2.24 flip: it was stub-callee Yellow under the body-level
// classification); `calls_calls_boom` is Yellow — Red travels arbitrarily
// far, unstubbability exactly one hop. `shields` is Red but BODY-level,
// so `calls_shields` stays Yellow, and `c_main` — whose callees are all
// Yellow, none Red — stays Green.
// CHECK:      item boom kind=function color=red reason=inadmissible construct=exceptions
// CHECK-NEXT: item c_main kind=function color=green reason=admissible
// CHECK-NEXT: item calls_boom kind=function color=red reason=red-callee via=boom edge=Calls chain=calls_boom->boom construct=exceptions
// CHECK-NEXT: item calls_calls_boom kind=function color=yellow reason=stub-callee via=calls_boom edge=Calls chain=calls_calls_boom->calls_boom->boom construct=exceptions
// CHECK-NEXT: item calls_shields kind=function color=yellow reason=stub-callee via=shields edge=Calls chain=calls_shields->shields construct=exceptions
// CHECK-NEXT: item shields kind=function color=red reason=inadmissible construct=exceptions
// CHECK-NEXT: tally green=1 yellow=2 red=3
// CHECK-NOT:  item
