// REQUIRES: cargo
// FR-80 differential end-to-end test: a LIBRARY crate that takes the
// ADDRESS of its const struct external global -- the lwIP
// `&ip_addr_any` / IP4_ADDR_ANY shape -- supplied by the consumer through
// an ADDRESS-CARRYING getter, `fn ip_addr_any() -> &'static IpAddr`.
//
// FR-79's by-value getter could not express pointer IDENTITY: `&g` needs a
// stable address, and a Copy temporary has a new one every call. The
// &'static item fixes exactly that: the consumer lends its one static item,
// so every `E::ip_addr_any()` is the SAME address, argument positions
// receive it directly, and the interior-member address projects through it.
// The functions below cover the measured lwIP demand: direct
// address-at-argument, the interior-member chain in both spellings
// (`&g.m` and `&((&g)->m)`), the address flowing through a local pointer,
// a deref-read through that pointer, and MIXED value-read plus
// address-take of the same global in one TU (one image serves both).
//
// The requirement FUNCTIONS (is_any, ip4_get) are the identity oracle: the
// native leg's C definition compares `p == &ip_addr_any`, the consumer's
// impl compares `core::ptr::eq(p, ...)`, and `check_other` hands both a
// DIFFERENT object holding the SAME VALUE when argc==1 -- so a lowering
// that erased identity into value equality byte-diffs, which a compile
// alone could never see. Both drivers seed from argc so constant folding
// cannot hide a miscompile.
//
// RUN: emitrust-cc --emit=crate %s -o %t.crate
// RUN: FileCheck --check-prefix=TRAIT %s < %t.crate/src/lib.rs
//
// The trait carries the two function requirements (their
// const-struct-pointee parameters borrow SHARED -- the shape a &'static
// can flow into) and the address-carrying getter; the &'static spelling
// exists ONLY in the trait item, call sites bind plain `&` values.
// TRAIT:      pub trait Externals {
// TRAIT-NEXT:     fn is_any(v0: &IpAddr) -> i32;
// TRAIT-NEXT:     fn ip4_get(v0: &Ip4) -> u32;
// TRAIT-NEXT:     fn ip_addr_any() -> &'static IpAddr;
// TRAIT-NEXT: }
// TRAIT-NOT:  set_ip_addr_any
// TRAIT:      pub fn check_any<E: Externals>(
// TRAIT-NEXT:     let v0: &IpAddr = E::ip_addr_any();
// TRAIT-NEXT:     E::is_any(v0)
//
// RUN: rustc --edition=2021 --crate-type=rlib \
// RUN:   --crate-name=lib_crate_externals_const_addr %t.crate/src/lib.rs \
// RUN:   -o %t.rlib
// RUN: rustc --edition=2021 \
// RUN:   --extern lib_crate_externals_const_addr=%t.rlib \
// RUN:   %S/Inputs/lib-crate-externals-const-addr-consumer.rs -o %t.consumer
// RUN: %t.consumer > %t.rust.out
//
// RUN: clang -std=c11 -DLIB_CRATE_MAIN %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: diff %t.native.out %t.rust.out

struct ip4 {
  unsigned int a;
};
struct ip_addr {
  struct ip4 u;
  int kind;
};

// Declared const, never defined here: the crate's requirement on its
// environment -- and this time its ADDRESS is what the code needs.
extern const struct ip_addr ip_addr_any;

// Requirement functions taking const-struct pointers: the consumer answers
// them, and the native leg's definitions use pointer IDENTITY.
int is_any(const struct ip_addr *p);
unsigned int ip4_get(const struct ip4 *q);

// The dominant lwIP shape: the requirement's address straight into a call.
int check_any(void) { return is_any(&ip_addr_any); }

// A DIFFERENT object with (at argc==1) the SAME VALUE: identity must say
// no where value equality would say yes.
int check_other(unsigned int seed) {
  struct ip_addr o = {{seed}, 4};
  return is_any(&o);
}

// The IP4_ADDR_ANY interior-member chain, in lwIP's arrow spelling and the
// plain dot spelling.
unsigned int member_chain(void) { return ip4_get(&((&ip_addr_any)->u)); }
unsigned int member_dot(void) { return ip4_get(&ip_addr_any.u); }

// The dhcp/udp local-pointer flow: bound to a local, then passed on.
int via_local(void) {
  const struct ip_addr *p = &ip_addr_any;
  return is_any(p);
}

// A deref-read through the taken address (value semantics through the
// same requirement).
unsigned int read_through(void) {
  const struct ip_addr *p = &ip_addr_any;
  return (unsigned int)p->kind + p->u.a;
}

// MIXED: value-read of the same global in a TU that also takes its
// address -- the one &'static getter serves both.
int mixed(int d) {
  struct ip_addr t = ip_addr_any;
  return t.kind + d + is_any(&ip_addr_any);
}

// The native oracle: the same driver the Rust consumer runs, plus the C
// definitions of the requirements. Compiled only for the native leg, so
// the transpiled project really does see them as undefined.
#ifdef LIB_CRATE_MAIN
int printf(const char *, ...);

const struct ip_addr ip_addr_any = {{0u}, 4};

int is_any(const struct ip_addr *p) { return p == &ip_addr_any; }
unsigned int ip4_get(const struct ip4 *q) { return q->a; }

int main(int argc, char **argv) {
  (void)argv;
  printf("check=%d\n", check_any());
  printf("other=%d\n", check_other((unsigned int)(argc - 1)));
  printf("chain=%u\n", member_chain() + (unsigned int)argc);
  printf("dot=%u\n", member_dot() * (unsigned int)argc);
  printf("local=%d\n", via_local());
  printf("read=%u\n", read_through() + (unsigned int)argc);
  printf("mixed=%d\n", mixed(argc));
  return 0;
}
#endif
