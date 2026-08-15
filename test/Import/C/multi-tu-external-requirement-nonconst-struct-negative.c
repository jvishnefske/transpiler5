// FR-81 boundaries: the non-const struct requirement admits ONLY whole-value
// loads and stores -- the shapes `E::g()` / `E::set_g(v)` can express. An
// ADDRESS into the global stays rejected: a mutable requirement address
// would need `&'static mut`, which the dyn-free/unsafe-free FR-52 contract
// excludes, and unlike FR-80's const case there is no shape-aware exception
// (`constStruct` keys both the address-taken gate and global_addr
// acceptance). This is the gate that keeps 9 of the 10 lwIP IP_DATA units
// rejected -- the ip.h macro family takes interior-member addresses
// (`&ip_data.current_iphdr_src`) -- so these pins are the measured frontier,
// not a hypothetical. Arrays of structs stay rejected too: element access
// needs a place-yielding projection no trait item yields.
// RUN: split-file %s %t
// RUN: not emitrust-import-c --externals-trait %t/member-addr-arg.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=MEMBER-ADDR-ARG %s
// RUN: not emitrust-import-c --externals-trait %t/member-addr-local.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=MEMBER-ADDR-LOCAL %s
// RUN: not emitrust-import-c --externals-trait %t/array.c \
// RUN:   %t/empty.c 2>&1 | FileCheck --check-prefix=ARRAY %s

//--- empty.c
int unrelated(int v) { return v; }

//--- member-addr-arg.c
// An interior-member address at an argument position (the lwIP
// `ip_current_src_addr()` macro shape) rejects EARLY, at the address-taking
// call site, with the pointer plan's wording -- before the requirement
// decision is ever reached.
struct ip_globals {
  int ttl;
  unsigned int addr;
};
extern struct ip_globals ip_data;
void sink(int *p);

void use_addr(void) { sink(&ip_data.ttl); }

int read_ttl(void) { return ip_data.ttl; }
// MEMBER-ADDR-ARG: member-addr-arg.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: passing a pointer into a global variable to a function

//--- member-addr-local.c
// An interior-member address the pointer plan SWALLOWS (bound to a local
// cursor, no surviving symbol use from the taking function): the
// whole-program addressTakenGlobals pre-scan still records the fact, and
// the finalize gate keeps the historical rejection -- located at the first
// SURVIVING use, the plain field read below. This is the backstop that
// makes the frontier an AST fact, not an accident of which uses an
// optimization happened to leave.
struct ip_globals {
  int ttl;
  unsigned int addr;
};
extern struct ip_globals ip_data;

int peek_via_ptr(void) {
  int *p = &ip_data.ttl;
  return *p;
}

int read_ttl(void) { return ip_data.ttl; }
// MEMBER-ADDR-LOCAL: member-addr-local.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'ip_data' is referenced but not defined in any translation unit

//--- array.c
// An ARRAY of non-const structs keeps the verbatim finalize rejection, like
// FR-79's const arrays: element access needs a place-yielding projection
// into environment-owned storage, which no trait item yields.
struct cfg {
  int a;
};
extern struct cfg table[4];

int first(void) { return table[0].a; }
// ARRAY: array.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: extern global variable 'table' is referenced but not defined in any translation unit
