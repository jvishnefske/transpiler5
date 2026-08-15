// FR-77: under --incremental, a fn-ptr constant naming a function no TU
// defines costs the CONTAINING item (the global) and, transitively, its
// readers -- never the crate. The invariant this pins: `Some(<name>)` is
// opaque text, not a SymbolUse, so before FR-77 this exact shape (tinycrypt
// ecc.c's `static uECC_RNG_Function g_rng_function = &default_CSPRNG;`,
// where the sibling TU defining the function is not on the compile line)
// sailed through import, the finalize walk erased the referenced prototype
// as "unused", and the emitted crate carried a dangling `Some(default_csprng)`
// -- rustc E0425, CRATE_NOBUILD, a violated `--incremental` contract. Now the
// rejection fires at the address-taking initializer, inside the per-decl
// recovery walk, so the global drops with an attributed blocker, its readers
// recover as stubs (the uECC_set_rng/get_rng shape), and everything else --
// including the cross-TU call into the companion TU -- still ports.
//
// RUN: emitrust-cc --emit=crate --incremental %s \
// RUN:   %S/Inputs/incremental-fnptr-undefined-other.c -o %t.crate 2>%t.err
// RUN: FileCheck %s --check-prefix=WARN --input-file=%t.err
// RUN: FileCheck %s --check-prefix=RUST --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=NONE --input-file=%t.crate/src/main.rs
// RUN: FileCheck %s --check-prefix=PORTING --input-file=%t.crate/PORTING.md
// RUN: FileCheck %s --check-prefix=JSON \
// RUN:   --input-file=%t.crate/emitrust-progress.json
//
// Recovery OFF is unchanged in shape: the same diagnostic as a hard error,
// a non-zero exit, and no output directory -- strict mode used to emit the
// broken crate too (exit 0, E0425 later), so this guard is half the fix.
// RUN: not emitrust-cc --emit=crate %s \
// RUN:   %S/Inputs/incremental-fnptr-undefined-other.c -o %t.strict.crate \
// RUN:   2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: not ls %t.strict.crate

typedef int (*RNG)(unsigned char *dest, unsigned int size);
extern int default_csprng(unsigned char *dest, unsigned int size);

// The initializer is the only reference to `default_csprng` anywhere on the
// compile line; the setter below keeps devirtualization from manufacturing a
// direct call (a SymbolUse), so only the FR-77 walk can see the dangling name.
// WARN: :[[#@LINE+1]]:{{[0-9]+}}: warning: unsupported: taking the address of undefined function 'default_csprng' (recovered: item dropped)
static RNG g_rng = &default_csprng;

void set_rng(RNG f) { g_rng = f; }
RNG get_rng(void) { return g_rng; }
int gen(unsigned char *buf, unsigned int n) {
  RNG f = g_rng;
  return f ? f(buf, n) : 0;
}

int describe(int n) { return n * 2 + 1; }
int offset(int x);

int main(void) { return describe(offset(3)); }

// The driver exits 0 and the summary attributes the drop to the global.
// WARN: dropped 'g_rng' [fnptr-undefined-target] unsupported: taking the address of undefined function 'default_csprng'

// Every translatable item reaches the crate -- including the companion TU's
// `offset` -- and the readers of the dropped global recover as stubs rather
// than dangling references.
// RUST-DAG: fn describe(
// RUST-DAG: fn offset(
// RUST-DAG: fn c_main(
// RUST-DAG: unimplemented!

// The dangling name and the dropped global leave NO trace in the crate:
// shipping either would be the E0425 miscompile this FR removes.
// NONE-NOT: default_csprng
// NONE-NOT: G_RNG

// The report names the construct that cost the global.
// PORTING: | fnptr-undefined-target | 1 |

// JSON: { "tag": "fnptr-undefined-target", "count": 1 }

// STRICT: error: unsupported: taking the address of undefined function 'default_csprng'
