// FR-83 (the FR-82 fold prerequisite): a `&` that is IMMEDIATELY cancelled
// by `->` or unary `*` — `(&g)->f`, `(*(&g)).f` — never materializes an
// address, so the whole-program pre-scan must NOT record `g` in
// `addressTakenGlobals`: the emission side already folds the pair into a
// plain member access, and marking the global anyway is what kept every
// lwIP unit with a macro-expanded `((struct ip_globals*)&ip_data)->...`
// access disqualified from the FR-81 requirement model (the measured
// ip6_frag whole-crate loss). The gate is SYNTACTIC and exact: only the
// deref-cancelled shape skips the mark; a REAL address-take — the same
// `&ip_data` bound to a pointer, or an interior-member address — still
// marks, and the requirement disqualification stays byte-for-byte
// (multi-tu-external-requirement-nonconst-struct-negative.c holds the
// interior-member arms).
// RUN: split-file %s %t
// RUN: emitrust-import-c --externals-trait %t/fold.c %t/empty.c \
// RUN:   | FileCheck --check-prefix=FOLD %s
// RUN: not emitrust-import-c --externals-trait %t/real-addr.c %t/empty.c \
// RUN:   2>&1 | FileCheck --check-prefix=REALADDR %s
// RUN: emitrust-import-c %t/defined.c | FileCheck --check-prefix=DEFINED %s

//--- empty.c
int unrelated(int v) { return v; }

//--- fold.c
// Both cancelled shapes on an extern-undefined non-const struct: the
// global stays requirement-qualified, and each access is the ordinary
// staged whole-value load + member on the temporary — identical to the
// plain `ip_data.ttl` spelling.
struct ip_globals {
  int ttl;
  unsigned int addr;
};
extern struct ip_globals ip_data;

// FOLD-LABEL: func.func @arrow_fold
// FOLD: emitrust.global_load @ip_data : !emitrust.struct<"ip_globals">
// FOLD: emitrust.member %{{.*}}["ttl"]
int arrow_fold(void) { return (&ip_data)->ttl; }

// FOLD-LABEL: func.func @deref_fold
// FOLD: emitrust.global_load @ip_data : !emitrust.struct<"ip_globals">
// FOLD: emitrust.member %{{.*}}["addr"]
unsigned deref_fold(void) { return (*(&ip_data)).addr; }

// FOLD-LABEL: func.func @arrow_fold_write
// FOLD: emitrust.global_load @ip_data
// FOLD: emitrust.member %{{.*}}["ttl"]
// FOLD: emitrust.global_store %{{.*}}, @ip_data
void arrow_fold_write(int t) { (&ip_data)->ttl = t; }

// The declaration survives as the FR-81 requirement global: the cancelled
// '&' did not disqualify it.
// FOLD: emitrust.global @ip_data {emitrust.external_requirement} : !emitrust.struct<"ip_globals">

//--- real-addr.c
// A REAL whole-global address-take (bound to a pointer local) still marks
// the global and keeps the historical rejection, located at the first
// surviving use — the fold gate must not widen past the exact cancelled
// shape.
struct ip_globals {
  int ttl;
  unsigned int addr;
};
extern struct ip_globals ip_data;

void escape(void) {
  struct ip_globals *p = &ip_data;
  (void)p;
}

int read_ttl(void) { return ip_data.ttl; }
// REALADDR: real-addr.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'ip_data' is referenced but not defined in any translation unit

//--- defined.c
// The fold on a DEFINED global (single TU, no flag): `(&g)->x` imports as
// the plain staged member read it always folded to — the pre-scan change
// only affects the marking, never the emission.
struct S {
  int x;
};

struct S g;

// DEFINED-LABEL: func.func @read_defined
// DEFINED: emitrust.global_load @g : !emitrust.struct<"S">
// DEFINED: emitrust.member %{{.*}}["x"]
int read_defined(void) { return (&g)->x; }
