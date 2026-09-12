## C99 Feature Coverage (in progress)

Same checkbox discipline as the MVP requirements: tick only when the
referenced regression tests pass under ninja check-emitrust.

- [x] C99-11 Aggregate initializer lists for arrays and structs,
  including nested and partially explicit initializers with implicit
  zeroing. Block scope: default-initialized place plus one
  `emitrust.assign` per explicit element (constant-index
  `emitrust.subscript` / `emitrust.member`), recursing for nested lists;
  file scope: clang-constant-evaluated APValue converted to a typed
  ArrayAttr element list on `emitrust.global`, verifier-checked against
  the array size / struct_def field count. See the full entry in
  "Declarations and initializers" above.
  (test/Import/C/aggregate-init.c, aggregate-init-invalid.c,
  test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/EndToEnd/aggregate-init.c)
- [x] C99-12 Designated initializers for array indices and struct fields.
  Implemented with C99-11 via clang's semantic initializer-list form
  (designators pre-resolved to positional elements with implicit-value
  holes). (test/Import/C/aggregate-init.c, test/EndToEnd/aggregate-init.c)
- [x] C99-14 File-scope objects: global variables with constant
  initializers, tentative definitions, extern declarations across
  translation units (single-TU first), and static file-scope objects
  (Rust mapping: static items; mutable globals are a design decision —
  no unsafe rules out static mut, pointing at interior-mutability
  wrappers or parameter threading).
  Implemented single-TU via module-level `emitrust.global` with
  `emitrust.global_load`/`emitrust.global_store` access ops. Never-written
  const-qualified globals emit plain `static NAME: T = INIT;` read
  directly; every other global emits a `thread_local!`
  `std::cell::Cell<T>` (all imported types are Copy) accessed with
  `.with(|c| c.get()/c.set(v))` — no `unsafe`, no `static mut`, exact for
  the single-threaded subset. No initializer means the type's default
  (C zero-initialization); scalar initializers are clang
  constant-evaluated; aggregate initializer lists are typed ArrayAttr
  element lists (C99-11); element/field access to global aggregates is
  load-modify-store of the whole value. Rejected with located
  diagnostics: taking a global's address in value position, Rust-keyword
  names, `_Thread_local`, extern-only declarations, and block-scope
  extern. Pointer-typed file-scope variables import through the CTS-P4
  global region model (single global base plus a stored i64 cursor
  global; see the c-testsuite checklist); pointer-typed function-local
  statics stay rejected.
  (test/Dialect/EmitRust/ops.mlir, invalid.mlir,
  test/Target/Rust/globals.mlir, test/Import/C/globals.c,
  globals-invalid.c, globals-keyword.c, globals-extern-only.c,
  aggregate-init.c, globals-thread-local.c, globals-pointer.c,
  globals-pointer-invalid.c,
  globals-extern-local.c, test/EndToEnd/globals.c)
- [x] C99-15 Static local variables preserving state across calls
  (design decision needed for a no-unsafe mapping).
  Implemented with the same `emitrust.global` machinery: a function-local
  static becomes a module-level global mangled `<function>_<name>`
  (collision with any existing module symbol is rejected), constant
  initializer required (C11 6.7.9p4, clang-enforced), initialized once at
  program start. (test/Import/C/globals.c,
  globals-static-collision.c, test/EndToEnd/globals.c)

