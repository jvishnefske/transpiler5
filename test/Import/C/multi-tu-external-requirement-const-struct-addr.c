// FR-80: an ADDRESS-CARRYING const-struct requirement. FR-79's by-value
// getter cannot express pointer identity, so `&g` on a qualifying extern
// const struct (the lwIP `&ip_addr_any` / IP4_ADDR_ANY shape) now lowers to
// `emitrust.global_addr @g : !emitrust.ref<...>` — a first-class shared
// reference the lowering pass rewrites to a `fn g() -> &'static T` trait
// getter call. This pin holds the whole qualifying surface:
//   * a body-less (prospective requirement) function's const-struct-pointee
//     pointer parameter borrows SHARED (`!emitrust.ref<...>`), the borrow
//     shape a consumer-supplied `&'static` can actually flow into (the
//     FR-75 precedent: requirement signatures carry the C contract);
//   * `&g` at such an argument position is a `global_addr`;
//   * the interior-member address (`&g.m` and lwIP's `&((&g)->m)` spelling)
//     projects through deref + member on the SAME `global_addr`, then
//     reborrows shared — never a staged copy, which would break identity;
//   * a local pointer bound to `&g` and passed on resolves to the same
//     `global_addr` (the dhcp/udp local `ip_addr_t *` flow);
//   * MIXED usage in one TU (value-read plus address-take) keeps the
//     whole-value `global_load` for the read: ONE image (the &'static
//     getter) serves both once lowered.
// The declaration survives with BOTH the const marker and the requirement
// marker; FR-70's addressTakenGlobals disqualifier is now SHAPE-AWARE: a
// const struct whose surviving uses are all loads/global_addrs qualifies,
// everything else (non-const, arrays, compares, stores-into-fields,
// returned addresses) keeps its verbatim rejection — pinned by
// multi-tu-external-requirement-const-struct-negative.c.
//
// RUN: emitrust-import-c --externals-trait %s \
// RUN:   %S/Inputs/multi-tu-empty.c | FileCheck %s

struct In {
  int v;
};
struct S {
  struct In m;
  int y;
};
extern const struct S cfg;

// Body-less under a trait policy: prospective requirements. The
// const-struct-pointee parameters borrow shared.
// CHECK-DAG: func.func private @take(!emitrust.ref<!emitrust.struct<"S">>) -> i32 attributes {emitrust.external_requirement}
// CHECK-DAG: func.func private @take_in(!emitrust.ref<!emitrust.struct<"In">>) -> i32 attributes {emitrust.external_requirement}
int take(const struct S *p);
int take_in(const struct In *q);

// Whole-object address at the argument position: the global_addr flows
// straight into the shared parameter.
// CHECK-LABEL: func.func @direct_arg
// CHECK: %[[A:.*]] = emitrust.global_addr @cfg : !emitrust.ref<!emitrust.struct<"S">>
// CHECK: call @take(%[[A]]) : (!emitrust.ref<!emitrust.struct<"S">>) -> i32
int direct_arg(void) { return take(&cfg); }

// Interior-member address, dot spelling: deref + member on the global_addr,
// reborrowed shared at the member's type.
// CHECK-LABEL: func.func @member_arg
// CHECK: %[[B:.*]] = emitrust.global_addr @cfg : !emitrust.ref<!emitrust.struct<"S">>
// CHECK: %[[P:.*]] = emitrust.deref %[[B]] : (!emitrust.ref<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"S">>
// CHECK: %[[M:.*]] = emitrust.member %[[P]]["m"] : (!emitrust.lvalue<!emitrust.struct<"S">>) -> !emitrust.lvalue<!emitrust.struct<"In">>
// CHECK: %[[R:.*]] = emitrust.addr_of %[[M]] : (!emitrust.lvalue<!emitrust.struct<"In">>) -> !emitrust.ref<!emitrust.struct<"In">>
// CHECK: call @take_in(%[[R]])
int member_arg(void) { return take_in(&cfg.m); }

// The same interior member in lwIP's IP4_ADDR_ANY spelling
// (`&((&g)->m)`): the arrow through the parenthesized address-of is the
// same projection.
// CHECK-LABEL: func.func @member_arg_arrow
// CHECK: emitrust.global_addr @cfg
// CHECK: emitrust.member {{.*}}["m"]
// CHECK: emitrust.addr_of
// CHECK: call @take_in
int member_arg_arrow(void) { return take_in(&((&cfg)->m)); }

// A local pointer bound to the address and passed on (the dhcp/udp local
// `ip_addr_t *` flow) resolves to the same global_addr at the call.
// CHECK-LABEL: func.func @via_local
// CHECK: emitrust.global_addr @cfg
// CHECK: call @take
int via_local(void) {
  const struct S *p = &cfg;
  return take(p);
}

// MIXED: the value-read in a TU that also takes the address stays a plain
// whole-value load — the lowering pass rewrites BOTH against the one
// &'static getter.
// CHECK-LABEL: func.func @mixed
// CHECK-DAG: emitrust.global_load @cfg : !emitrust.struct<"S">
// CHECK-DAG: emitrust.global_addr @cfg
int mixed(void) {
  struct S t = cfg;
  return t.y + take(&cfg);
}

// The declaration survives at module end with BOTH markers: const is what
// licenses the shared lend, the requirement marker is what routes it to
// the lowering pass (and makes any survivor fail loudly at emission).
// CHECK: emitrust.global const @cfg {emitrust.external_requirement} : !emitrust.struct<"S">
