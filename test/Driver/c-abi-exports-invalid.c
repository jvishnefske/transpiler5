// FR-139: `--c-abi-exports` is meaningful only where a LIBRARY crate is
// produced, so every other spelling is a clear error rather than a flag that
// is silently ignored. A user who asked for dlopen-able exports and got an
// rlib of mangled `pub fn` back has no way to tell from the artifact; the
// failure has to happen here, at the command line, not at dlsym time in
// somebody else's harness.
//
// Three rejections are pinned:
//   * the crate SHAPE is a binary one -- whether chosen by `--crate-type=bin`
//     or by the default `auto` seeing a `main`. A binary has no exported
//     surface at all (FR-51 keeps a bin crate's items private), so a C-ABI
//     export request over it is meaningless. The message is LOCATED on the
//     module and names the way out.
//   * the emission mode produces no crate root at all.
//   * `--partition`, whose workspace members are path dependencies of each
//     other: turning a member into a cdylib would break the very link the
//     workspace exists to make, so the combination is refused instead of
//     quietly emitting one or the other.
//
// This input defines `main`, so the default shape is a binary crate.
// RUN: not emitrust-cc --emit=crate --c-abi-exports %s -o %t.auto 2>&1 \
// RUN:   | FileCheck %s --check-prefix=AUTOBIN
//
// The explicit spelling gives the same located error, not a different one.
// RUN: not emitrust-cc --emit=crate --crate-type=bin --c-abi-exports %s \
// RUN:   -o %t.bin 2>&1 | FileCheck %s --check-prefix=AUTOBIN
//
// ...and `--emit=rust`, which prints the crate ROOT, is held to it too.
// RUN: not emitrust-cc --emit=rust --crate-type=bin --c-abi-exports %s \
// RUN:   -o %t.rs 2>&1 | FileCheck %s --check-prefix=AUTOBIN
//
// Forcing the library shape on the same input is accepted: `c_main` becomes
// an ordinary all-scalar export.
// RUN: emitrust-cc --emit=crate --crate-type=lib --c-abi-exports %s \
// RUN:   -o %t.lib
// RUN: cat %t.lib/src/lib.rs | FileCheck %s --check-prefix=LIB
//
// No crate root, no crate shape to export from.
// RUN: not emitrust-cc --emit=mlir --c-abi-exports %s -o /dev/null 2>&1 \
// RUN:   | FileCheck %s --check-prefix=BADMODE
//
// RUN: not emitrust-cc --emit=crate --partition --c-abi-exports %s \
// RUN:   -o %t.ws 2>&1 | FileCheck %s --check-prefix=BADPART

int printf(const char *, ...);

int doubled(int x) { return x * 2; }

int main(void) {
  printf("%d\n", doubled(21));
  return 0;
}

// AUTOBIN: error: --c-abi-exports needs a library crate, but this input emits a BINARY one. Pass --crate-type=lib to export its functions instead

// The forced library really does export both functions the C ABI way.
// LIB:      #[no_mangle]
// LIB-NEXT: pub extern "C" fn doubled(x: i32) -> i32 {
// LIB:      #[no_mangle]
// LIB-NEXT: pub extern "C" fn c_main() -> i32 {

// BADMODE: error: --c-abi-exports is only valid with --emit=crate or --emit=rust

// BADPART: error: --c-abi-exports does not apply under --partition: a workspace member is a path dependency of its siblings, which a cdylib cannot be
