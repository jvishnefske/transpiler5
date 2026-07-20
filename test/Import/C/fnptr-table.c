// CTS-BR (00216): function-pointer TABLES and void* fn-ptr members.
// (1) A never-written global array of function pointers imports as an
// `!emitrust.array<Nx!emitrust.fn_ptr<...>>` global whose folded
// element list honors designator overrides (the 00216 [0 ... 2] range
// pre-fill is fully overridden by the per-index designators); an
// indexed call `table[i]()` is an ordinary subscript load feeding
// `emitrust.call_indirect`. (2) A struct member declared `void *` whose
// every stored value across the TU is the address of a function of ONE
// signature (the T1.1 holder bound: never reassigned after aggregate
// initialization) imports as a fn_ptr member; reading it into a
// fn-ptr local and calling through the local works with both a global
// and a local array of such structs, and with both the compound-literal
// and the bare-address initializer element spellings.
//
// RUN: emitrust-import-c %s | FileCheck %s

int printf(const char *, ...);

void sys_ni(void) { printf("ni\n"); }
void sys_one(void) { printf("one\n"); }
void sys_two(void) { printf("two\n"); }
void sys_three(void) { printf("three\n"); }

typedef void (*fptr)(void);
const fptr table[3] = {
    [0 ... 2] = &sys_ni,
    [0] = sys_one,
    [1] = sys_two,
    [2] = sys_three,
};
// CHECK: emitrust.global const @table <[#emitrust.opaque<"Some(sys_one)">, #emitrust.opaque<"Some(sys_two)">, #emitrust.opaque<"Some(sys_three)">]> : !emitrust.array<3x!emitrust.fn_ptr<()>>

void test_multi_relocs(void) {
  int i;
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    table[i]();
}
// CHECK-LABEL: func.func @test_multi_relocs
// CHECK: emitrust.call_indirect %{{.*}}() : (!emitrust.fn_ptr<()>) -> ()

struct Wrap {
    void *func;
};
// CHECK: emitrust.struct_def @Wrap ["func"] [!emitrust.fn_ptr<()>]
int global;
void inc_global(void) {
  global++;
}

struct Wrap global_wrap[] = {
    ((struct Wrap) {inc_global}),
    inc_global,
};
// CHECK: emitrust.global @global_wrap <{{\[}}[#emitrust.opaque<"Some(inc_global)">], [#emitrust.opaque<"Some(inc_global)">]]> : !emitrust.array<2x!emitrust.struct<"Wrap">>

void test_compound_with_relocs(void) {
  struct Wrap local_wrap[] = {
      ((struct Wrap) {inc_global}),
      inc_global,
  };
  void (*p)(void);
  p = global_wrap[0].func;
  p();
  p = global_wrap[1].func;
  p();
  p = local_wrap[0].func;
  p();
  p = local_wrap[1].func;
  p();
}
// CHECK-LABEL: func.func @test_compound_with_relocs
// CHECK-DAG: emitrust.variable : !emitrust.lvalue<!emitrust.array<2x!emitrust.struct<"Wrap">>>
// CHECK-DAG: emitrust.variable : !emitrust.lvalue<!emitrust.fn_ptr<()>>
// Reading the void* member yields the fn_ptr value; no cast op is
// needed for the assignment to p.
// CHECK-DAG: emitrust.member %{{.*}}["func"] : {{.*}} -> !emitrust.lvalue<!emitrust.fn_ptr<()>>
// CHECK: emitrust.call_indirect
// CHECK: emitrust.call_indirect
// CHECK: emitrust.call_indirect
// CHECK: emitrust.call_indirect

int main(void) {
  test_compound_with_relocs();
  test_multi_relocs();
  printf("%d\n", global);
  return 0;
}
