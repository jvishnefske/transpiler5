// RUN: split-file %s %t
// RUN: emitrust-cc --emit=rust %t/program.c -o - | FileCheck %s --check-prefix=PROG
// RUN: emitrust-cc --emit=rust %t/library.c -o - | FileCheck %s --check-prefix=LIB
// RUN: emitrust-cc --emit=rust %t/library.c -o - | FileCheck %s --check-prefix=LIBNOT
// RUN: emitrust-cc --emit=rust %t/silent.c -o - | FileCheck %s --check-prefix=SILENT --strict-whitespace --match-full-lines
// RUN: emitrust-cc --emit=rust %t/abort.c -o - | FileCheck %s --check-prefix=ABORT
// RUN: emitrust-cc --emit=rust %t/print-only.c -o - | FileCheck %s --check-prefix=PRINTONLY
// RUN: not emitrust-import-c %t/underscore-exit.c 2>&1 | FileCheck %s --check-prefix=UNDEREXIT
// RUN: not emitrust-import-c %t/quick-exit.c 2>&1 | FileCheck %s --check-prefix=QUICKEXIT
// RUN: not emitrust-import-c %t/raise.c 2>&1 | FileCheck %s --check-prefix=RAISE

// FR-228: the SHAPE pins for C's stdout buffering model. What each of these
// asserts is a decision the runtime makes at IMPORT time; the byte-diff that
// says the decisions are RIGHT lives in test/EndToEnd/stdout-buffering-*.c,
// and nothing here is a substitute for it.
//
// The model: C's `stdout` is FULLY buffered when it is not an interactive
// device and is flushed at `exit` or when the buffer fills. Rust's `Stdout`
// is a `LineWriter`. The two agree for every NORMAL termination and diverge
// for every termination that does not flush, so the crate now carries its own
// writer and every stdout write is routed through it.

//--- program.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  printf("v=%d", argc);
  printf("\n");
  if (argc > 1)
    exit(2);
  return 0;
}

// The runtime is the crate's FIRST item and it has to be: `macro_rules!`
// scoping is TEXTUAL, so a `print!` shadow covers only the uses that follow
// it, and a use that misses the shadow falls back silently to std's macro --
// whose LineWriter would then interleave with the buffered writer and reorder
// the program's output. The Rust emitter hoists these items to the top of the
// crate root whatever their IR position, which is what makes a `--link` merge
// (where the runtime rides in whichever shard happened to print) agree with a
// joint import.
// PROG:      macro_rules! print {
// PROG-NEXT:     ($($arg:tt)*) => { crate::__emitrust_out_fmt(format_args!($($arg)*)) };
// PROG-NEXT: }

// The buffer's fill/flush rule is glibc's `_IO_new_file_xsputn`, not a
// `BufWriter`'s: it FILLS the buffer and flushes only when a write no longer
// fits, then writes whole blocks straight through. A `BufWriter` flushes the
// bytes it already holds instead, which puts a different number of bytes on
// disk at every boundary past the first (measured against the clang native:
// one write of 4096 -> 4096 on disk, two of 4096 -> 4096, 8192 -> 8192,
// 4097 -> 4096; `BufWriter` matches only the first).
// PROG: const __EMITRUST_STDOUT_BUFSIZ: usize = 4096;

// Buffering is opt-in AT RUNTIME, and only the entry wrapper opts in. That is
// what makes the failure direction safe: a module that never reaches a `fn
// main` -- a `--crate-type=lib` build of this very file, say -- keeps the
// unbuffered writes it has today rather than filling a buffer nobody flushes.
// PROG: fn __emitrust_stdout_init() {
// PROG: if std::io::stdout().is_terminal() {
// PROG: let inner = std::panic::take_hook();

// `std::process::exit` runs NO destructors, so a buffer dropped by it is
// never written. Every C `exit(status)` therefore gets the flush spliced in
// ahead of it at import; missing one would be SILENT TRUNCATION, which is
// strictly worse than the divergence this FR set out to fix.
// PROG:      crate::__emitrust_out_flush();
// PROG-NEXT: std::process::exit(2i32);

//--- library.c
#include <stdio.h>
void report(int n) { printf("n=%d\n", n); }

// A module with no `c_main` is a LIBRARY: it runs inside a process whose exit
// it does not own, so there is nowhere to flush from and a buffer would be
// silent truncation. It gets the SAME runtime text all the same -- and the
// sameness is the point. A `--link` merge dedups header verbatims by exact
// printed text, so a per-shard decision about the runtime's SHAPE produced
// two `fn __emitrust_out_write` definitions and rustc E0428 the moment one
// shard of a program had the `c_main` and another did not (measured on
// test/EndToEnd/link-slice-model-e2e.c).
//
// What makes the library safe is that buffering is a RUNTIME flag which only
// the entry wrapper sets, so a crate that never reaches one keeps today's
// direct writes. `__emitrust_stdout_init` is then genuinely uncalled here and
// still needs no dead-code cover: rustc's lint skips every item whose name
// begins with an underscore, which is what has always exempted the
// `__emitrust_` namespace (probed).
// LIB:      macro_rules! println {
// LIB:      static __EMITRUST_STDOUT_FULL: std::sync::atomic::AtomicBool =
// LIB-NEXT:     std::sync::atomic::AtomicBool::new(false);
// LIB:      fn __emitrust_out_write(bytes: &[u8]) {
// LIB-NEXT:     use std::io::Write;
// LIB-NEXT:     if !__EMITRUST_STDOUT_FULL.load(std::sync::atomic::Ordering::Relaxed) {
// LIB-NEXT:         std::io::stdout().write_all(bytes).expect("stdout write failed");
// LIB-NEXT:         return;
// LIB-NEXT:     }
// A library has no entry wrapper, so nothing ever turns buffering on.
// LIBNOT-NOT: __emitrust_stdout_init();

//--- silent.c
#include <stdlib.h>
int main(void) {
  int a = 3;
  if (a > 2)
    exit(1);
  return 0;
}

// A program that writes NOTHING to stdout gets no runtime, no flush and the
// historical entry wrapper, byte for byte: there is no buffer, so there is
// nothing for the exit to flush. This is what keeps the change confined to
// crates that actually print. Pinned full-width so an accidental preamble
// cannot slip in.
// SILENT:fn c_main() -> i32 {
// SILENT-NEXT:    std::process::exit(1i32);
// SILENT-NEXT:}
// SILENT-EMPTY:
// SILENT-NEXT:fn main() { std::process::exit(c_main()); }

//--- abort.c
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  printf("before abort %d\n", argc);
  if (argc > 0)
    abort();
  return 0;
}

// FR-224 refused `abort` because the emitted crate printed bytes the clang
// native did not. `std::process::abort` always matched C's abort exactly --
// same SIGABRT, same 134, no destructors and NO FLUSH -- and the last of
// those was the problem only because the crate's own stdout was
// line-buffered. With C's model in place the refusal is gone and the call
// lowers, and the ABSENCE of a flush before it is the entire point: a flush
// here would write MORE than glibc does, not less.
// The CHECK-NEXT chain is the load-bearing part: it says there is NOTHING
// between the print and the abort, so no flush was spliced in.
// ABORT:      fn c_main(argc: i32) -> i32 {
// ABORT-NEXT:     println!("before abort {}", argc);
// ABORT-NEXT:     if argc > 0i32 {
// ABORT-NEXT:         std::process::abort();
// ABORT-NEXT:     }

//--- print-only.c
#include <stdio.h>
int main(void) {
  printf("no newline");
  return 0;
}

// Only the macros the crate actually uses are emitted -- this one never
// completes a line, so it gets `print!` and `__emitrust_out_fmt` and neither
// of the `println` pair. Emitting both unconditionally would put rustc's
// `unused_macros` on a large share of the corpus; the decision is read off
// the IR (`emitrust.call_opaque "print!"` / `"println!"`), which is exactly
// what the Rust emitter renders, so it cannot miss a print path.
// PRINTONLY:     macro_rules! print {
// PRINTONLY:     fn __emitrust_out_fmt(args: std::fmt::Arguments) {
// PRINTONLY-NOT: macro_rules! println
// PRINTONLY-NOT: __emitrust_out_line

// THE THREE OTHER NON-FLUSHING TERMINATIONS, and why FR-228 did NOT admit
// them. All three still take the generic system-header refusal -- located,
// which is the acceptable floor -- and the wording is pinned here so that a
// later wave that admits one has to say so.
//
// `_Exit` and `quick_exit` are ALMOST free now. With C's buffering model in
// place, the emitted crate's unflushed bytes sit in ITS buffer, so lowering
// `_Exit(n)` to `std::process::exit(n)` WITHOUT the flush splice would
// reproduce C exactly with stdout redirected to a file (measured: `printf
// ("complete\n"); printf("partial"); _Exit(3);` writes 0 bytes through
// clang+glibc). It is the TERMINAL case that stops it: on a tty glibc
// line-buffers, so C writes `complete\n` and DROPS `partial` (measured, 10
// bytes through a pty), while the emitted crate hands every write straight
// to std's `Stdout` when it is a terminal and Rust's `process::exit` flushes
// that -- so `partial` would appear. Admitting `_Exit` therefore requires the
// writer to own the terminal case too (line-buffered in the same sense), not
// just the redirected one. That is a real follow-on, not a line of code, and
// a divergence in the "prints bytes the native does not" direction is exactly
// what FR-228 exists to remove.
//
// `raise` is further away: Rust std has no `raise`, `raise(SIGABRT)` is only
// abort-equivalent while no handler is installed, and proving that is a
// whole-program question about `signal`.
//--- underscore-exit.c
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  printf("before\n");
  _Exit(3);
}
// UNDEREXIT: underscore-exit.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to '_Exit' declared in a system header; not part of the supported C subset

//--- quick-exit.c
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  printf("before\n");
  quick_exit(3);
}
// QUICKEXIT: quick-exit.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'quick_exit' declared in a system header; not part of the supported C subset

//--- raise.c
#include <signal.h>
#include <stdio.h>
int main(void) {
  printf("before\n");
  raise(SIGABRT);
  return 0;
}
// RAISE: raise.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: call to 'raise' declared in a system header; not part of the supported C subset
