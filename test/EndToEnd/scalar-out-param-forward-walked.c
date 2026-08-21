// REQUIRES: cargo
// FR-100: pins the byte-level correctness of a WALKING-CURSOR slice
// argument composed with a forwarded scalar out-parameter in the SAME
// call — the two classes FR-100's fixpoint must keep apart, exercised
// where the slice argument's cursor is NOT zero.
// `walk` advances its own byte cursor by a parameter-dependent amount
// and only THEN calls `tail`, which writes through what it received,
// while `total` is threaded through untouched as a scalar reference. If
// the reslice at the advanced cursor were ever dropped — the failure
// mode the arm's SliceType exclusion is written against — `tail` would
// write at index 0 instead of at the cursor, and the printed buffer
// would differ. That is invisible to `cargo build` (the types match
// either way) and invisible to an IR CHECK on the signature, so the
// pin is a stdout byte-diff against the clang-built native.
// HONEST SCOPE, measured by mutation: this test does NOT pin the
// `!isa<SliceType>(pointee)` guard itself. That guard is unreachable —
// the slice-argument branch above it (ImportCExpressions.cpp:4596-4857)
// ends in an unconditional return, so a slice pointee never reaches the
// forwarding arm — and deleting the guard leaves this test passing.
// The guard is deliberate defense in depth, not a live condition; what
// this test pins is the composition's emitted BYTES.
// Every offset derives from argc, so constant folding cannot pre-compute
// the answer; the extra RUN pairs re-seed via argv words (argv itself is
// never read). All writes in bounds, no UB.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/scalar_out_param_forward_walked > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: %t.native a > %t.native2.out
// RUN: %t.crate/target/release/scalar_out_param_forward_walked a > %t.rust2.out
// RUN: diff %t.native2.out %t.rust2.out
// RUN: %t.native a b c > %t.native4.out
// RUN: %t.crate/target/release/scalar_out_param_forward_walked a b c > %t.rust4.out
// RUN: diff %t.native4.out %t.rust4.out

#include <stdio.h>

/* Writes through the run it was handed; `total` is a pure out-param. */
static void tail(unsigned char *run, int value, int *total) {
    run[0] = (unsigned char)value;
    run[1] = (unsigned char)(value + 1);
    *total += run[0] + run[1];
}

/* Walks its own cursor first, so the argument it forwards is a slice at
   a NON-ZERO offset -- the case the exclusion exists for. */
static void walk(unsigned char *buf, int skip, int value, int *total) {
    int i;
    for (i = 0; i < skip; i++) {
        buf++;
    }
    tail(buf, value, total);
}

static int sum_from(const unsigned char *buf, int skip) {
    int i;
    int acc = 0;
    for (i = 0; i < skip; i++) {
        buf++;
    }
    acc += buf[0];
    acc += buf[1];
    return acc;
}

int main(int argc, char **argv) {
    unsigned char buf[12];
    int total = 0;
    int i;
    int skip = argc + 1;

    for (i = 0; i < 12; i++) {
        buf[i] = (unsigned char)(200 + i);
    }

    walk(buf, skip, argc * 7, &total);
    walk(buf, skip + 3, argc * 11, &total);

    printf("total=%d\n", total);
    printf("tail_sum=%d\n", sum_from(buf, skip));
    for (i = 0; i < 12; i++) {
        printf("%d ", (int)buf[i]);
    }
    printf("\n");
    return 0;
}
