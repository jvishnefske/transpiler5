// REQUIRES: cargo
// FR-153 differential: a `T **` cursor parameter that forwards `*p` on to
// a callee needing a MUTABLE region borrow. This is systemd's
// `notify_on_cleanup` shape verbatim (a `const char **` cursor handing
// its region to `sd_notify(int, const char *)`), and it was the last
// rustc error class blocking the whole-program systemd crate: the cursor
// parameter's region base was pushed as a SHARED `&[i8]` while the body
// reborrowed it mutably to build the forwarded argument, so the emitted
// crate died with `error[E0596]: cannot borrow *p as mutable, as it is
// behind a & reference`.
//
// The fix is demand-driven: the base borrows mutably IFF the callee's own
// body forwards `*p` to a parameter that maps to a mutable borrow. This
// test is the runtime half of that pin — `cargo build` success is
// compile-only and cannot see a miscompile, so stdout is byte-diffed
// against the clang-built native binary. Seeds derive from argc so the
// cursor offset, the forwarded byte, and the returned value cannot
// constant-fold.
//
// Note the caller's region here is a STRING LITERAL's read-only backing:
// a mutable region parameter rematerializes a fresh non-const backing
// (writing through a pointer to a string literal is undefined behavior,
// so the per-call copy is unobservable to any defined program). The
// dedicated pin for that fence is cursor-region-demand-mut-literal.c.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate --crate-name cursor_region_demand_mut --build
// RUN: clang -std=c11 -w %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/cursor_region_demand_mut > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

int printf(const char *, ...);

// `const char *` still maps to a MUTABLE byte slice parameter, so
// forwarding a cursor's region here is what demands the mutable base.
static int sd_notify(int unset, const char *state) {
  return (int)state[0] + unset;
}

// The cursor parameter: reads under `*p` only, forwards `*p` onward.
static int notify_on_cleanup(const char **p, int unset) {
  if (*p)
    return sd_notify(unset, *p);
  return -1;
}

int main(int argc, char **argv) {
  int seed = argc - 1;
  const char *s = "Rx";
  /* argc-derived cursor: the forwarded region view starts at a
     non-constant offset. */
  s = s + seed;
  int r = notify_on_cleanup(&s, seed);
  printf("r=%d s=%c\n", r, *s);
  return 0;
}
