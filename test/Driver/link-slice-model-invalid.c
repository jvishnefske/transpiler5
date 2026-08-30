// FR-158 phase 1, the FRONTIER of the link-time slice-model
// reconciliation: everything the merge does NOT know how to reshape stays a
// LOCATED rejection.
//
// The merge may rewrite a declaring shard's call arguments only when the
// declaration and the definition differ by exactly
// `!emitrust.mut_ref<T>` -> `!emitrust.mut_ref<!emitrust.slice<T>>` on some
// parameters AND the argument at each such slot is the
// `addr_of mut (subscript base[index])` cursor a C `&arr[k]` imports to.
// Measured over the 501-object systemd link, that covers 3796 of 3796
// diverging declarations and 14303 of 14363 mismatched argument slots. This
// file pins what happens to the rest -- because the alternative is not "a
// slightly worse crate", it is rustc E0308 with no source location, which
// is the exact failure FR-158 exists to remove.
//
// The legs:
//   ARITY  -- the declaration has a different parameter COUNT. Today this
//             merges silently and emits a call with the wrong number of
//             arguments; it must be refused.
//   RESULT -- the declaration has a different RESULT type.
//   SCALAR -- the argument is the address of a scalar OBJECT, not of an
//             array element (50 of the 60 residual systemd slots). C's
//             `f(&c)` gives the callee a one-element region; nothing here
//             knows that, so it is refused rather than widened.
//   MEMBER -- the argument is the address of a struct FIELD (the other 10).
//   FNPTR  -- the slice-refined function's symbol appears in an
//             `emitrust.opaque` `Some(f)` fn-pointer payload. systemd has
//             ZERO of these, so this leg is synthetic BY CONSTRUCTION and
//             is the only thing standing between the guard and a silently
//             wrong function-pointer type: the table's element type is
//             built from the DECLARATION, and adapting the calls alone
//             would leave it behind.
//   ARGS   -- an `emitrust.call_opaque` carrying a positional `args`
//             remap, where operand index is not argument position. Not
//             reachable from C (the attribute exists for Rust macro calls),
//             so the shard is hand-written MLIR fed through the sidecar
//             path.
//
// And one POSITIVE leg the rewrite must not break:
//   SHARED -- ONE `addr_of` value feeding TWO callees, only one of which is
//             slice-refined. The rewrite adds a `slice_of` for the refined
//             call and may drop the `addr_of` only when it becomes
//             use-free; here the second callee still needs it, so it must
//             survive. Also hand-written: the importer materializes a
//             separate cursor per call site, so a genuinely shared
//             `addr_of` cannot be spelled in C.
//
// The per-TU `-D` is load-bearing: with identical import args FR-58's joint
// link-time RE-IMPORT rescues these pairs in the importer and none of the
// merge-level code under test runs.

// RUN: split-file %s %t
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/def-arith.c -DTU_A=1 -o %t/def-arith.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/def-struct.c -DTU_A=1 -o %t/def-struct.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-arity.c -DTU_B=1 -o %t/use-arity.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-result.c -DTU_B=1 -o %t/use-result.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-scalar.c -DTU_B=1 -o %t/use-scalar.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-member.c -DTU_B=1 -o %t/use-member.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %t/use-fnptr.c -DTU_B=1 -o %t/use-fnptr.o

// The two hand-written shards go straight onto the link line: `--link`
// accepts a `.mlirbc`/`.mlir` shard named directly, and a link-line input
// that is not an object file IS the shard payload (LinkMerge's documented
// three-way `findShardPayload` contract).

// ARITY
// RUN: not emitrust-cc --link %t/def-arith.o %t/use-arity.o --crate-type=lib --emit=rust -o %t/arity.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ARITY
// ARITY: use-arity.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'note' is declared as '(!emitrust.mut_ref<i8>, i32, i32) -> ()' but the defining translation unit defines it as '(!emitrust.mut_ref<!emitrust.slice<i8>>, i32) -> ()'
// ARITY: def-arith.c:{{[0-9]+}}:{{[0-9]+}}: note: defined here

// RESULT
// RUN: not emitrust-cc --link %t/def-arith.o %t/use-result.o --crate-type=lib --emit=rust -o %t/result.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RESULT
// RESULT: use-result.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'note' is declared as '(!emitrust.mut_ref<i8>, i32) -> i32' but the defining translation unit defines it as '(!emitrust.mut_ref<!emitrust.slice<i8>>, i32) -> ()'
// RESULT: def-arith.c:{{[0-9]+}}:{{[0-9]+}}: note: defined here

// SCALAR -- located at the C CALL SITE, naming the argument.
// RUN: not emitrust-cc --link %t/def-arith.o %t/use-scalar.o --crate-type=lib --emit=rust -o %t/scalar.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SCALAR
// SCALAR: use-scalar.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument 1 of the call to 'note' is a scalar reference but the defining translation unit classifies that parameter as a slice
// SCALAR: def-arith.c:{{[0-9]+}}:{{[0-9]+}}: note: 'note' is defined here as '(!emitrust.mut_ref<!emitrust.slice<i8>>, i32) -> ()'

// MEMBER
// RUN: not emitrust-cc --link %t/def-arith.o %t/use-member.o --crate-type=lib --emit=rust -o %t/member.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=MEMBER
// MEMBER: use-member.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: argument 1 of the call to 'note' is a scalar reference but the defining translation unit classifies that parameter as a slice

// FNPTR
// RUN: not emitrust-cc --link %t/def-struct.o %t/use-fnptr.o --crate-type=lib --emit=rust -o %t/fnptr.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=FNPTR
// FNPTR: use-fnptr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: 'snote' is used as a value here but the defining translation unit classifies its parameter 1 as a slice, so this use carries a function-pointer type the definition does not have

// ARGS
// RUN: not emitrust-cc --link %t/def-arith.o %t/args-shard.mlir --crate-type=lib --emit=rust -o %t/args.rs 2>&1 \
// RUN:   | FileCheck %s --check-prefix=ARGS
// ARGS: error: unsupported: the call to 'note' remaps its arguments positionally, so it cannot be adapted to the defining translation unit's slice parameter model

// SHARED -- the positive: the refined call gets a `slice_of`, and the
// `addr_of` the unrefined call still uses survives.
// RUN: emitrust-cc --link %t/def-arith.o %t/shared-shard.mlir --crate-type=lib --emit=rust -o %t/shared.rs
// RUN: FileCheck %s --check-prefix=SHARED < %t/shared.rs
// SHARED-DAG: &mut [i8] = &mut b[2i32 as usize..]
// SHARED-DAG: &mut i8 = &mut b[2i32 as usize]{{;$}}
// SHARED-DAG: note(
// SHARED-DAG: peek(

//--- def-arith.c
/* `note` subscripts its pointer parameter, so the importer classifies it
   as a slice; `peek` only dereferences, so it stays a scalar reference. */
void note(char *s, int n) {
  for (int i = 0; i < n; i++)
    s[i] = (char)(s[i] + 1);
}

int peek(char *p) { return *p; }

//--- def-struct.c
struct P {
  char c;
  int v;
};

void snote(struct P *s, int n) {
  for (int i = 0; i < n; i++)
    s[i].c = (char)(s[i].c + 1);
}

//--- use-arity.c
void note(char *, int, int);

int arity_caller(void) {
  char b[4] = "abc";
  note(&b[1], 2, 3);
  return 0;
}

//--- use-result.c
int note(char *, int);

int result_caller(void) {
  char b[4] = "abc";
  return note(&b[1], 2);
}

//--- use-scalar.c
void note(char *, int);

int scalar_caller(void) {
  char c = 'x';
  note(&c, 1);
  return 0;
}

//--- use-member.c
struct S {
  char a;
  char b;
};

void note(char *, int);

int member_caller(void) {
  struct S s;
  s.a = 'q';
  s.b = 'r';
  note(&s.a, 1);
  return 0;
}

//--- use-fnptr.c
/* A STRUCT pointee keeps the importer's address-taken forcing (which
   promotes an address-taken function's arithmetic-pointee scalar
   parameters to slices) from hiding the divergence: `snote` here is
   declared `!emitrust.mut_ref<!emitrust.struct<"P">>` while the defining
   TU slices it. */
struct P {
  char c;
  int v;
};

void snote(struct P *, int);

static void (*tbl[2])(struct P *, int) = {snote, 0};

int fnptr_caller(int k) {
  struct P a[2];
  a[0].c = 'x';
  a[0].v = 1;
  a[1].c = 'y';
  a[1].v = 2;
  tbl[k](&a[0], 2);
  return 0;
}

//--- args-shard.mlir
module {
  emitrust.func private @note(!emitrust.mut_ref<i8>, i32) attributes {emitrust.extern_decl}
  emitrust.func @args_caller() -> i32 {
    %c0 = emitrust.constant <0 : i32> : i32
    %c2 = emitrust.constant <2 : i32> : i32
    %b = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.array<8xi8>>
    %e = emitrust.subscript %b[%c2] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i32) -> !emitrust.lvalue<i8>
    %a = emitrust.addr_of mut %e : (!emitrust.lvalue<i8>) -> !emitrust.mut_ref<i8>
    emitrust.call_opaque "note"(%a, %c2) {args = [0 : index, 1 : index]} : (!emitrust.mut_ref<i8>, i32) -> ()
    emitrust.return %c0 : i32
  }
}

//--- shared-shard.mlir
module {
  emitrust.func private @note(!emitrust.mut_ref<i8>, i32) attributes {emitrust.extern_decl}
  emitrust.func private @peek(!emitrust.mut_ref<i8>) -> i32 attributes {emitrust.extern_decl}
  emitrust.func @shared_caller() -> i32 {
    %c2 = emitrust.constant <2 : i32> : i32
    %b = emitrust.variable named "b" : !emitrust.lvalue<!emitrust.array<8xi8>>
    %e = emitrust.subscript %b[%c2] : (!emitrust.lvalue<!emitrust.array<8xi8>>, i32) -> !emitrust.lvalue<i8>
    %a = emitrust.addr_of mut %e : (!emitrust.lvalue<i8>) -> !emitrust.mut_ref<i8>
    emitrust.call_opaque "note"(%a, %c2) : (!emitrust.mut_ref<i8>, i32) -> ()
    %r = emitrust.call_opaque "peek"(%a) : (!emitrust.mut_ref<i8>) -> i32
    emitrust.return %r : i32
  }
}
