// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/explicit-null.c 2>&1 | FileCheck %s --check-prefix=EXPLICIT
// RUN: not emitrust-import-c %t/sentinel.c 2>&1 | FileCheck %s --check-prefix=SENTINEL
// RUN: not emitrust-import-c %t/short-init.c 2>&1 | FileCheck %s --check-prefix=SHORT
// RUN: not emitrust-import-c %t/designated-hole.c 2>&1 | FileCheck %s --check-prefix=DESIGNATED
// RUN: not emitrust-import-c %t/enum-gap.c 2>&1 | FileCheck %s --check-prefix=ENUMGAP
// RUN: not emitrust-import-c %t/null-test.c 2>&1 | FileCheck %s --check-prefix=NULLTEST
// RUN: not emitrust-import-c %t/row-compare.c 2>&1 | FileCheck %s --check-prefix=ROWCMP

// FR-217 MISCOMPILE FENCE 1. A `const char *const` string table lowers to
// the padded byte form `[[i8; W]; N]`, which has NO image for a NULL
// element: padding one to `""` would flip `!!t[i]` from false to true and
// `t[i][0]` from undefined behavior to `'\0'`. That is a silently wrong
// ANSWER, not a missing feature, so every shape that puts a NULL into the
// table refuses with a LOCATED diagnostic naming the offending index --
// and the diagnostic must stay exactly this specific, because the whole
// value of the fence is that a reader can tell which element killed the
// table.
//
// FR-215 measured three such tables in the systemd corpus (virt.c:167,
// catalog.c:31, pager.c:304); the sentinel-terminated form below is the
// idiom all three use. C99 6.7.8p21's zero fill puts the same NULL into a
// short or designated initializer without anyone writing one, so those
// two spellings are pinned here as well.
//
// The last two files pin the OTHER half of the fence: even for a table
// with no NULL in it, a row must never reach a pointer-VALUE context. The
// padded rows have no addresses to compare or test, so `while (t[i])` and
// `t[i] == t[j]` must refuse rather than silently answer against padding.
// Without this, a table admitted by the lowering could still make a
// NULL-terminated walk run off its end.

// EXPLICIT: explicit-null.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null element at index 2 of string table 'names' (the padded lowering would turn it into "")

//--- explicit-null.c
int printf(const char *, ...);
static const char *const names[4] = {"a", "bb", 0, "dd"};
int main(void) {
  printf("%s", names[0]);
  return 0;
}

// The sentinel-terminated idiom: the trailing `NULL` is the loop bound.
// SENTINEL: sentinel.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null element at index 2 of string table 'names' (the padded lowering would turn it into "")

//--- sentinel.c
int printf(const char *, ...);
static const char *const names[] = {"x", "y", (void *)0};
int main(void) {
  printf("%s", names[0]);
  return 0;
}

// C99 6.7.8p21 zero-fills the tail of a short initializer, so index 2 is a
// NULL nobody wrote.
// SHORT: short-init.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null element at index 2 of string table 'names' (the padded lowering would turn it into "")

//--- short-init.c
int printf(const char *, ...);
static const char *const names[3] = {"p", "q"};
int main(void) {
  printf("%s", names[0]);
  return 0;
}

// A designated initializer leaves a NULL in the middle, and the fence must
// name THAT index (1), not the first or the last.
// DESIGNATED: designated-hole.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null element at index 1 of string table 'names' (the padded lowering would turn it into "")

//--- designated-hole.c
int printf(const char *, ...);
static const char *const names[3] = {[0] = "p", [2] = "r"};
int main(void) {
  printf("%s", names[0]);
  return 0;
}

// The real-world shape of the hole, and the reason this fence is not a
// corner case: systemd's DEFINE_STRING_TABLE_LOOKUP tables are indexed by
// an enum and sized by its _MAX, and several DELIBERATELY leave one
// enumerator unmapped (src/cryptenroll/cryptenroll.c:145 says so in a
// comment: "ENROLL_PASSWORD has no entry here"). The whole point of that
// table is that the missing slot reads back NULL, so padding it to "" is
// precisely the wrong answer.
// ENUMGAP: enum-gap.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: null element at index 0 of string table 'token_type_table' (the padded lowering would turn it into "")

//--- enum-gap.c
int printf(const char *, ...);
enum EnrollType {
  ENROLL_PASSWORD,
  ENROLL_RECOVERY,
  ENROLL_PKCS11,
  _ENROLL_TYPE_MAX,
};
/* ENROLL_PASSWORD has no entry here. */
static const char *const token_type_table[_ENROLL_TYPE_MAX] = {
    [ENROLL_RECOVERY] = "systemd-recovery",
    [ENROLL_PKCS11] = "systemd-pkcs11",
};
int main(void) {
  printf("%s", token_type_table[1]);
  return 0;
}

// A NULL-terminated walk over a table that HAS no NULL: the loop condition
// asks a pointer question the padded rows cannot answer, so it refuses
// instead of looping forever (or off the end).
// NULLTEST: null-test.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: row of string table 'names' used as a pointer value (the padded lowering gives its rows no address)

//--- null-test.c
int printf(const char *, ...);
static const char *const names[2] = {"a", "b"};
int main(void) {
  int i = 0;
  while (names[i]) {
    printf("%s", names[i]);
    i++;
  }
  return 0;
}

// Pointer identity between two rows is likewise unanswerable: in C these
// are two distinct literal objects, and the padded form has no addresses
// at all.
// ROWCMP: row-compare.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: row of string table 'names' used as a pointer value (the padded lowering gives its rows no address)

//--- row-compare.c
int printf(const char *, ...);
static const char *const names[2] = {"a", "b"};
int main(void) {
  printf("%d", names[0] == names[1]);
  return 0;
}
