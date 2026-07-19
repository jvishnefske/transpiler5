// REQUIRES: cargo
// C99-45 (c-testsuite 00218 core): an enum : 8 bit-field with bit 7 set
// must zero-extend on read. The stored code is computed from argc so no
// constant folder can pre-decide the switch: 151 + argc == AMBIG_CONV
// (152) at runtime, and the switch in convert_like_real must take the
// AMBIG_CONV arm — a sign-extending read would see -104 and fall through
// to the "broken" printf, diverging from the native binary. Also mirrors
// 00218's one-arm union carrying the struct, the Rust-keyword member
// `type`, ->common.code chains, address-of-struct traffic, and a
// flag write that must not clobber the neighbouring enum field.
// RUN: emitrust-cc --emit=crate %s -o %t.crate --build
// RUN: clang -std=c11 %s -o %t.native
// RUN: %t.native > %t.native.out
// RUN: %t.crate/target/release/bitfields_zeroextend > %t.rust.out
// RUN: diff %t.native.out %t.rust.out

extern int printf(const char *, ...);

enum tree_code {
  SOME_CODE = 148, /* bit 7 set, and hence all further enum values too */
  LAST_AND_UNUSED_TREE_CODE
};
struct tree_common {
  union tree_node *chain;
  union tree_node *type;
  enum tree_code code : 8;
  unsigned side_effects_flag : 1;
};
union tree_node {
  struct tree_common common;
};
typedef union tree_node *tree;
enum c_tree_code {
  C_DUMMY_TREE_CODE = LAST_AND_UNUSED_TREE_CODE,
  STMT_EXPR,
  LAST_C_TREE_CODE
};
enum cplus_tree_code {
  CP_DUMMY_TREE_CODE = LAST_C_TREE_CODE,
  AMBIG_CONV, /* == 152 */
  LAST_CPLUS_TREE_CODE
};

int matched(void) { return 41; }

int convert_like_real(tree convs) {
  switch ((enum tree_code)(convs)->common.code) {
  case AMBIG_CONV:
    return matched();
  default:
    break;
  }
  printf("unsigned enum bit-fields broken\n");
}

int main(int argc, char **argv) {
  union tree_node convs;

  convs.common.code = (enum tree_code)(151 + argc); /* AMBIG_CONV at runtime */
  convs.common.side_effects_flag = (unsigned)(argc & 1);
  printf("code %d\n", (int)convs.common.code); /* 152, never -104 */
  printf("flag %u\n", convs.common.side_effects_flag);
  printf("conv %d\n", convert_like_real(&convs));

  /* Rewriting the 1-bit neighbour must leave the enum field intact. */
  convs.common.side_effects_flag = (unsigned)((argc + 1) & 1);
  printf("code2 %d\n", (int)convs.common.code);
  printf("flag2 %u\n", convs.common.side_effects_flag);
  return 0;
}
