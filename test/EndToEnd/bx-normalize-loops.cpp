// REQUIRES: cargo
// FR-254: emitrust-bx --normalize, C++ -> Rust, byte-diffed against clang++
// of the ORIGINAL (un-normalized) source -- the rewrite must not change
// behaviour. Every loop here is outside the subset without --normalize: R1
// rewrites a non-canonical `for` (stride, descending `!=` / `>`, step-less,
// `for (;; c *= 2)`, `for (; v >= 128U; v >>= 7)`, an unsigned counter, a
// two-variable init, an expression init whose counter is read after the
// loop) to `{ I; while (C) { B S; } }`, and R2 rewrites do-while to
// `while (true) { B if (!(C)) { break; } }`. The load-bearing rows: `break`
// and `return` out of a rewritten loop (both forms skip the step), a
// `continue` bound to a NESTED canonical loop inside a rewritten one, nested
// rewrites, and two CAPTURE rows -- a step and a do-while condition naming an
// outer variable that the body shadows; a naive splice would read the inner
// one and print different numbers. UB-free (accumulators reduced mod 10007,
// bounded counters, shifts of unsigned values by < 32), inputs from an LCG.
// RUN: %emitrust-bx --normalize --emit=crate %s -o %t.crate --build
// RUN: clang++ -std=c++17 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bx_normalize_loops > %t.rust.out
// RUN: diff %t.native.out %t.rust.out
// RUN: not grep -q unsafe %t.crate/src/main.rs
#include <cstdio>

int next(int x) {
    return (x * 1103 + 12345) % 30011;
}

int strided(int n, int stride) {
    int acc = 7;
    for (int i = -3; i < n; i += stride) {
        acc = (acc * 5 + i) % 10007;
    }
    return acc;
}

int descending(int n) {
    int acc = 1;
    for (int i = n; i != 0; --i) {
        acc = (acc * 3 + i) % 10007;
    }
    for (int i = n + 2; i > -4; i--) {
        acc = (acc * 7 + i) % 10007;
    }
    return acc;
}

int stepless(int n) {
    int acc = 0;
    for (int i = 0; i < n;) {
        acc = (acc * 9 + i) % 10007;
        i += acc % 3 + 1;
    }
    return acc;
}

int doubling(int lim) {
    int acc = 0;
    for (int c = 1;; c *= 2) {
        if (c > lim) {
            break;
        }
        acc = (acc + c) % 10007;
    }
    return acc;
}

unsigned varint_len(unsigned v) {
    unsigned len = 1U;
    for (; v >= 128U; v >>= 7) {
        len += 1U;
    }
    return len;
}

int unsigned_counter(unsigned u) {
    int acc = 0;
    for (unsigned k = u; k > 0U; k /= 3U) {
        acc = (acc * 4 + static_cast<int>(k % 11U)) % 10007;
    }
    return acc;
}

int two_vars(int n) {
    int acc = 0;
    for (int i = 0, j = n; i < j; ++i) {
        acc = (acc * 6 + j - i) % 10007;
        j -= 1;
    }
    return acc;
}

int expression_init(int n) {
    int i = 0;
    int acc = 0;
    for (i = n; i > 0; i -= 4) {
        acc = (acc * 2 + i) % 10007;
    }
    return acc * 10 + i;
}

int first_over(int n, int lim) {
    for (int i = n;; i += 5) {
        if (i > lim) {
            return i;
        }
    }
    return -1;
}

int early_break(int n) {
    int acc = 0;
    for (int i = 0; i < n; i += 2) {
        if (acc % 11 == 5) {
            break;
        }
        acc = (acc * 8 + i) % 10007;
    }
    return acc;
}

int nested(int n) {
    int acc = 0;
    for (int i = n; i > 0; i -= 3) {
        for (int j = 0; j < 4; ++j) {
            if (j == 2) {
                continue;
            }
            acc = (acc * 3 + j + i) % 10007;
        }
        for (int k = i; k > 0; k /= 2) {
            acc = (acc + k) % 10007;
        }
        int d = i;
        do {
            acc = (acc * 10 + d % 10) % 10007;
            d /= 10;
        } while (d != 0);
    }
    return acc;
}

int step_capture(int n) {
    int acc = 0;
    int t = 3;
    for (int i = 0; i < n; i += t) {
        int t = i % 4 + 10;
        acc = (acc * 2 + t) % 10007;
    }
    return acc;
}

int cond_capture(int n) {
    int acc = 0;
    int k = 2;
    int x = 0;
    do {
        int k = x % 5;
        acc = (acc * 3 + k) % 10007;
        x += 1;
    } while (x < n + k);
    return acc;
}

int digits(int n) {
    int count = 0;
    do {
        count += 1;
        n /= 10;
    } while (n != 0);
    return count;
}

int do_break(int n) {
    int acc = 0;
    int x = n;
    do {
        if (x > 50) {
            break;
        }
        acc = (acc + x) % 10007;
        x += 7;
    } while (x < 2 * n + 20);
    return acc;
}

int main() {
    int x = 17;
    for (int r = 0; r < 6; ++r) {
        x = next(x);
        int n = x % 41;
        std::printf("strided %d %d %d\n", n, strided(n, 2), strided(n, 5));
        std::printf("descending %d stepless %d doubling %d\n", descending(n), stepless(n), doubling(n * 13));
        std::printf("varint %u %u counter %d\n", varint_len(static_cast<unsigned>(x) * 977U),
                    varint_len(static_cast<unsigned>(n)), unsigned_counter(static_cast<unsigned>(x)));
        std::printf("two %d expr %d first %d early %d\n", two_vars(n), expression_init(n), first_over(n, 60),
                    early_break(n));
        std::printf("nested %d step %d cond %d digits %d %d do %d\n", nested(n), step_capture(n), cond_capture(n),
                    digits(x), digits(0), do_break(n));
    }
    return 0;
}
