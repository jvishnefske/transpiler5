// C99-17: a flexible array member (C99 6.7.2.1p16, `T tail[];`) gives the
// struct an allocation-time size the fixed-shape value model cannot
// represent — a documented rejection (c-testsuite 00216's first blocker).
// The dedicated wording is pinned here so the FAM shape never degrades to
// the generic array fallback (`unsupported: non-constant array size`).
// RUN: not emitrust-import-c %s 2>&1 | FileCheck %s

struct S {
  int n;
  int tail[];
};

int g(struct S *p);

// CHECK: flexible-array-invalid.c:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: flexible array member
