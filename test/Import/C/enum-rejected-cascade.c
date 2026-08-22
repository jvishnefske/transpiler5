// RUN: split-file %s %t
// RUN: emitrust-cc --recover --emit=rust %t/kw.c 2>%t.err | FileCheck %s --check-prefix=KW \
// RUN:     --implicit-check-not="Match" --implicit-check-not="struct Holder"
// RUN: FileCheck %s --check-prefix=KWDIAG --input-file=%t.err

// FR-113 recovery hardening: `importEnum` used to memoize into
// `importedEnums` BEFORE any rejection check, so a rejected enum looked
// already-imported on every later visit and `mapType` handed back an
// `!emitrust.enum<...>` with no enum_def behind it. Measured on unpatched
// HEAD, this keyword-named enum under --recover shipped a crate SILENTLY
// (exit 0) containing `pub m: Match` with no `Match` definition anywhere --
// cargo build then failed E0425 with zero attribution, the exact FR-50
// silent-breakage class the rejectedRecords doc comment describes for
// records. The fix is the same failure-sticky memo (`rejectedEnums`,
// FR-118's pattern): every later reference to the rejected enum cascades
// with a LOCATED `enum 'X' was rejected, so a type naming it cannot be
// imported`, through BOTH choke points -- mapType's enum branch (the struct
// field below) and emitEnumConstant (the bare enumerator use below) -- so
// the users drop attributably and the emitted crate carries NO trace of the
// rejected enum. The cascade wording auto-tabulates as
// [rejected-type-cascade]; the definition's own drop carries the FR-113
// [enum-def-rejected] tag.

//--- kw.c
enum match { m_lo = 1, m_hi = 2 };

/* The mapType channel: a field whose type names the rejected enum. */
struct holder {
  enum match m;
  int pad;
};

int type_user(void) {
  struct holder h;
  h.pad = 3;
  return h.pad;
}

/* The emitEnumConstant channel: a bare enumerator of the rejected enum. */
int const_user(void) {
  return m_lo + 1;
}

int survivor(int n) { return n + 1; }

// KW: fn survivor

// The definition's own located rejection, then the located cascades at
// each use site -- never a silent half-import.
// KWDIAG: kw.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: enum name 'match' is a Rust keyword (recovered: item dropped)
// KWDIAG: kw.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: enum 'match' was rejected, so a type naming it cannot be imported (recovered: item dropped)
// KWDIAG: kw.c:{{[0-9]+}}:{{[0-9]+}}: warning: unsupported: enum 'match' was rejected, so a type naming it cannot be imported (recovered:
// KWDIAG-NOT: error:
