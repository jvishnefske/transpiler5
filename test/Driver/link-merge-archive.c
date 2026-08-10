// FR-58 slice 2, static archive members on the link line: a positional
// input that is an `ar` archive is EXPANDED IN PLACE — every member
// object's `.emitrust` payload is extracted, in archive order, as if the
// members had been listed at that position on the link line. A member
// carrying no payload (a real build may archive hand-written assembly
// objects the shim never saw) is skipped with a warning and the link still
// succeeds. Uses --emit=rust so no cargo is needed; the member objects are
// produced through the FR-56 shim exactly as a real build would.
//
// Shim-built objects; the library object goes into an archive:
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %s -o %t.main.o
// RUN: env EMITRUST_REAL_CC=clang emitrust-clang -c %S/Inputs/link-merge-archive-lib.c -o %t.lib.o
// RUN: rm -f %t.lib.a
// RUN: llvm-ar rcs %t.lib.a %t.lib.o
//
// Linking the loose object against the ARCHIVE merges items from both:
// RUN: emitrust-cc --link %t.main.o %t.lib.a --emit=rust -o %t.rs
// RUN: FileCheck %s --check-prefix=MERGED < %t.rs
// MERGED-DAG: fn main()
// MERGED-DAG: fn add(
//
// The archive link is byte-identical to naming the member loose at the
// same position on the link line:
// RUN: emitrust-cc --link %t.main.o %t.lib.o --emit=rust -o %t.loose.rs
// RUN: diff %t.loose.rs %t.rs
//
// Payload-less member: strip the `.emitrust` section from a copy of the
// library object and archive BOTH copies. The bare member is skipped with a
// warning (archive members have no sidecar to fall back to), the payload
// member still resolves `add`, and the output is unchanged:
// RUN: llvm-objcopy --remove-section %emitrust_section_spec %t.lib.o %t.bare.o
// RUN: rm -f %t.mixed.a
// RUN: llvm-ar rcs %t.mixed.a %t.bare.o %t.lib.o
// RUN: emitrust-cc --link %t.main.o %t.mixed.a --emit=rust -o %t.mixed.rs 2>%t.warn.txt
// RUN: FileCheck %s --check-prefix=WARN < %t.warn.txt
// RUN: diff %t.mixed.rs %t.rs
// WARN: warning: archive member '{{.*}}bare.o' of '{{.*}}mixed.a' has no .emitrust payload; skipped

int add(int a, int b);

int main(void) { return add(40, 2); }
