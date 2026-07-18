#!/usr/bin/env python3
"""Deterministic seed-to-C-program generator for the differential fuzzer.

``generate_program(seed)`` is a PURE function of ``random.Random(seed)``:
no wall clock, no ``os.urandom``, and every random choice draws from an
explicitly ordered list, so the same seed always yields a byte-identical
program (see GENERATOR_VERSION for the determinism contract).

Each program composes 2-5 feature templates -- one helper-function block
per template instance plus a call site in ``main`` -- threaded through a
single unsigned accumulator.  Every template receives its inputs as a
function parameter derived from a loop-computed ``salt``, so indices and
branch conditions are runtime values that constant folding cannot cheat
on.  Programs print running printf digests at multiple points and return
a computed value in 0..250 (``acc % 251``).

``generate_program`` also returns the program's EXPECTED stdout bytes
and exit code, computed by an exact per-template Python evaluator
(``evaluate_plan``) with explicit wrapping arithmetic -- the third leg
of differ.py's three-way comparison.  No template is oracle-exempt.

Templates mirror the executable spec in test/EndToEnd/*.c: union puns
(unions.c), byte reinterprets over char arrays (pointers-reinterpret.c),
cell-slice globals with permuted recursion (pointers-global-args.c),
void*/member-base/null-ternary pointers (pointers-void.c,
pointers-member-base.c, pointers-null-ternary.c), variadic
extra-dropping + sprintf formats (varargs-def.c, sprintf.c), statement
expressions (stmt-expr.c), fn-ptr devirtualization (fnptr-devirt.c),
global-return chains (pointers-return-global.c), and int-carrier +
__builtin_expect + missing-return combos
(missing-return-expect-carrier.c), and writeback ordering -- stores into
a global whose RHS or index expression calls a helper mutating a
DISTINCT subobject of that same global (globals-writeback-order.c).  A
configurable fraction of seeds
(CROSS_FRACTION) is forced to combine at least two pointer-provenance
templates.

UB discipline: all array indices are masked/modded against known sizes,
arithmetic that could overflow is performed in unsigned, no object is
read uninitialized, byte puns stay within the 4-byte run over their
char array, and every generated construct appears verbatim in a passing
test/EndToEnd differential test.
"""

import argparse
import random
import sys
from collections import namedtuple

# Bump when the seed->program mapping changes; a campaign result is only
# reproducible against the same generator version.
# Version history: 1 = initial nine templates; 2 = writeback_order added
# (RHS/index calls mutating a distinct subobject of the assigned global);
# 3 = three-way expected-output oracle (same seed->program mapping as 2,
# but the Program artifact now includes evaluator-computed expectations).
GENERATOR_VERSION = "3"

# The expected-output oracle simulates the byte puns with native (host)
# endianness; both differential legs run on the same host, and that host
# is assumed little-endian.  Fail loudly otherwise.
if sys.byteorder != "little":
    raise RuntimeError(
        "genprog's expected-output oracle assumes a little-endian host;"
        " sys.byteorder=%r" % sys.byteorder
    )

# Fraction of seeds forced to combine >= 2 pointer-provenance templates.
CROSS_FRACTION = 0.5

# Width-extreme / sparse / negative constants threaded into templates.
# INT_MIN itself is excluded (negating it is UB); INT_MIN+1 stands in.
POOL_INT = [-1, 0, 1, -2147483647, 2147483647, 0x55AA, 0xAA55, -21847]
POOL_SMALL = [-1, 0, 1, 7, 42, -9]
POOL_MASK = [0x55AA55AA, 0xAA55AA55, 0x01010101, 0x7FFFFFFF, 0x00FF00FF]

Instance = namedtuple("Instance", ["uid", "name", "params"])
Plan = namedtuple("Plan", ["seed", "warm", "seed_mix", "instances"])
Program = namedtuple(
    "Program",
    ["source", "template_names", "plan", "expected_stdout", "expected_exit"],
)


def _int_lit(value):
    """Render a Python int as a plain C int literal (parenthesized if negative)."""
    if value < 0:
        return "(%d)" % value
    return "%d" % value


def _uns_lit(value):
    """Render a Python int as a C unsigned literal (two's-complement wrapped)."""
    return "%du" % (value & 0xFFFFFFFF)


# ---------------------------------------------------------------------------
# Template renderers.  Each takes (uid, params) and returns the C text of the
# template's globals + helper functions.  The call site in main is always
# ``acc = tmpl_<name>_u<uid>(acc);``.
# ---------------------------------------------------------------------------


def _render_union_pun(uid, p):
    u = "u%d" % uid
    return """union Pun_%(u)s { int i; unsigned u; };
struct Hold_%(u)s { int tag; union Pun_%(u)s p; };

static unsigned tmpl_union_%(u)s(unsigned salt) {
  union Pun_%(u)s v;
  struct Hold_%(u)s h;
  unsigned acc = salt;
  int j;
  for (j = 0; j < %(iters)d; j++) {
    v.u = acc ^ %(mask)s;
    printf("%(u)s.a=%%d\\n", v.i);
    v.u = (unsigned)%(pool_a)s ^ (salt %% 13u);
    acc = acc + v.u + (unsigned)j;
    printf("%(u)s.b=%%d acc=%%u\\n", v.i, acc %% 100000u);
  }
  v.i = -(int)(salt %% 100u) - %(negoff)d;
  printf("%(u)s.n=%%u\\n", v.u);
  acc = acc + v.u;
  h.tag = (int)(salt %% 17u);
  h.p.u = (unsigned)%(pool_b)s ^ (salt %% 9u);
  printf("%(u)s.h=%%d %%d %%u\\n", h.tag, h.p.i, h.p.u);
  acc = acc + h.p.u + (unsigned)h.tag;
  return acc;
}
""" % {
        "u": u,
        "iters": p["iters"],
        "mask": _uns_lit(p["mask"]),
        "pool_a": _int_lit(p["pool_a"]),
        "pool_b": _int_lit(p["pool_b"]),
        "negoff": p["negoff"],
    }


def _render_byte_pun(uid, p):
    u = "u%d" % uid
    length = p["length"]
    arr = "gb_%s" % u if p["use_global"] else "lb"
    local_decl = "" if p["use_global"] else "  char lb[%d];\n" % length
    global_decl = "char gb_%s[%d];\n\n" % (u, length) if p["use_global"] else ""
    return """%(global_decl)sstatic unsigned tmpl_bytes_%(u)s(unsigned salt) {
%(local_decl)s  int j;
  unsigned off;
  unsigned w;
  unsigned acc = salt;
  for (j = 0; j < %(length)d; j++)
    %(arr)s[j] = (char)('A' + (j + (int)(salt %% 7u)) %% 26);
  printf("%(u)s.i j=%%d\\n", (int)(salt %% 7u));
  for (j = 0; j < 3; j++) {
    off = (unsigned)(j * (%(length)d - 4)) / 2u;
    w = *(unsigned *)(%(arr)s + off);
    acc = acc * 31u + (w %% 65536u);
    *(unsigned *)(%(arr)s + off) = w ^ %(mask)s;
    printf("%(u)s.w j=%%d off=%%u w=%%u\\n", j, off, w %% 100000u);
  }
  off = salt %% %(modspan)du;
  *(unsigned *)(%(arr)s + off) += %(delta)s ^ (salt | 1u);
  w = *(unsigned *)(%(arr)s + off);
  printf("%(u)s.c off=%%u w=%%u\\n", off, w %% 100000u);
  acc = acc + w;
  for (j = 0; j < %(length)d - 1; j++)
    %(arr)s[j] = (char)('a' + (int)(((unsigned)%(arr)s[j] & 255u) %% 26u));
  %(arr)s[%(length)d - 1] = 0;
  printf("%(u)s.s=%%s\\n", %(arr)s);
  return acc;
}
""" % {
        "global_decl": global_decl,
        "local_decl": local_decl,
        "u": u,
        "arr": arr,
        "length": length,
        "modspan": length - 3,
        "mask": _uns_lit(p["mask"]),
        "delta": _uns_lit(p["delta"]),
    }


_CELL_PERMS = [("s, sp, d", "sp, d, s"), ("s, d, sp", "sp, s, d"), ("sp, s, d", "s, d, sp")]


def _render_cell_slice(uid, p):
    u = "u%d" % uid
    perm1, perm2 = _CELL_PERMS[p["perm"]]
    return """int CA_%(u)s[4];
int CB_%(u)s[4];
int CC_%(u)s[4];

static void cpr_%(u)s(void) {
  int i;
  printf("%(u)s.st");
  for (i = 0; i < 4; i++)
    printf(" %%d", CA_%(u)s[i]);
  printf(" |");
  for (i = 0; i < 4; i++)
    printf(" %%d", CB_%(u)s[i]);
  printf(" |");
  for (i = 0; i < 4; i++)
    printf(" %%d", CC_%(u)s[i]);
  printf("\\n");
}

static int cmv_%(u)s(int *s, int *d) {
  int i = 0;
  int j = 0;
  while (i < 4 && s[i] == 0)
    i++;
  while (j < 4 && d[j] == 0)
    j++;
  if (i >= 4 || j <= 0) {
    printf("%(u)s.skip\\n");
    return 0;
  }
  d[j - 1] = s[i];
  s[i] = 0;
  cpr_%(u)s();
  return d[j - 1];
}

static void chn_%(u)s(int n, int *s, int *d, int *sp) {
  if (n <= 1) {
    cmv_%(u)s(s, d);
    return;
  }
  chn_%(u)s(n - 1, %(perm1)s);
  cmv_%(u)s(s, d);
  chn_%(u)s(n - 1, %(perm2)s);
}

static unsigned tmpl_cells_%(u)s(unsigned salt) {
  int j;
  int n;
  int m;
  for (j = 0; j < 4; j++) {
    CA_%(u)s[j] = j + 1 + (int)(salt %% 3u);
    CB_%(u)s[j] = 0;
    CC_%(u)s[j] = 0;
  }
  n = CA_%(u)s[1] - CA_%(u)s[0] + %(extra)d;
  printf("%(u)s.n=%%d\\n", n);
  cpr_%(u)s();
  m = cmv_%(u)s(CA_%(u)s, CC_%(u)s);
  printf("%(u)s.m=%%d\\n", m);
  m = m + cmv_%(u)s(CC_%(u)s, CA_%(u)s);
  chn_%(u)s(n, CA_%(u)s, CB_%(u)s, CC_%(u)s);
  cpr_%(u)s();
  return salt * 3u + (unsigned)(CA_%(u)s[0] + CB_%(u)s[3] + CC_%(u)s[3] + m + 64);
}
""" % {
        "u": u,
        "perm1": perm1,
        "perm2": perm2,
        "extra": p["extra"],
    }


def _render_ptr_mix(uid, p):
    u = "u%d" % uid
    return """struct MB_%(u)s { int a; int b; int c; };
struct MB_%(u)s gmb_%(u)s;

static unsigned tmpl_ptrs_%(u)s(unsigned salt) {
  int x;
  int y;
  void *pp;
  void *vv;
  int *r;
  int *q;
  int *q2;
  int i;
  unsigned acc = salt;
  unsigned uu;
  x = (int)(salt %% 100u) + %(base)d;
  y = (int)(salt %% 30u) + 1;
  pp = &x;
  for (i = 0; i < %(iters)d; i++) {
    *(int *)pp = *(int *)pp + i;
    acc = acc + (unsigned)*(int *)pp;
    q = ((acc %% 2u) == 0u) ? &x : 0;
    if (q) {
      *q = *q + 1;
      printf("%(u)s.q=%%d\\n", *q);
    } else {
      printf("%(u)s.null i=%%d\\n", i);
    }
  }
  vv = pp;
  r = (int *)vv;
  *r = *r + (int)(acc %% 50u);
  printf("%(u)s.x=%%d\\n", x);
  gmb_%(u)s.a = (int)(salt %% 40u);
  gmb_%(u)s.b = %(pool_small)s;
  gmb_%(u)s.c = 3;
  q2 = &y;
  if (gmb_%(u)s.a > %(thresh)d)
    q2 = &gmb_%(u)s.b;
  *q2 = *q2 + gmb_%(u)s.a;
  printf("%(u)s.mb=%%d %%d %%d y=%%d\\n", gmb_%(u)s.a, gmb_%(u)s.b, gmb_%(u)s.c, y);
  acc = acc + (unsigned)y;
  uu = *(unsigned *)pp;
  uu = uu * 2654435761u;
  acc = acc + uu;
  printf("%(u)s.u=%%u\\n", uu %% 100000u);
  return acc + (unsigned)gmb_%(u)s.b;
}
""" % {
        "u": u,
        "iters": p["iters"],
        "base": p["base"],
        "pool_small": _int_lit(p["pool_small"]),
        "thresh": p["thresh"],
    }


# sprintf format matrix: (format piece, C argument expression template).
# Argument expressions are pure and bounded; %(lit)s is a pool constant.
_FMT_MATRIX = [
    ("[%d]", "(int)((unsigned)%(lit)s ^ (salt %% 31u))"),
    ("[%5d]", "-(int)(salt %% 90u) - 7"),
    ("[%-6d]", "(int)(salt %% 1000u) - 500"),
    ("[%05d]", "-(int)(salt %% 500u)"),
    ("[%u]", "(unsigned)%(lit)s + (salt %% 65536u)"),
    ("[%x]", "(unsigned)%(lit)s | (salt %% 255u)"),
    ("[%08x]", "(unsigned)%(lit)s ^ (salt %% 4096u)"),
    ("[%02u]", "salt %% 7u"),
    ("[%c]", "(char)('a' + (int)(salt %% 26u))"),
    ("[%s]", None),  # string literal, filled per-instance
]


def _render_va_sprintf(uid, p):
    u = "u%d" % uid
    pieces = []
    args = []
    for slot in ("fmt_a", "fmt_b", "fmt_c"):
        fmt, expr = _FMT_MATRIX[p[slot]]
        pieces.append(fmt)
        if expr is None:
            args.append('"s%s"' % u)
        else:
            args.append(expr % {"lit": _int_lit(p["val_" + slot[-1]])})
    return """static int vf_%(u)s(int a, int b, ...) {
  if (a > b)
    return a - b;
  return b - a + 1;
}

static unsigned tmpl_va_%(u)s(unsigned salt) {
  char buf[96];
  int n;
  int m;
  unsigned acc = salt;
  n = sprintf(buf, "%(fmt)s", %(args)s);
  printf("%(u)s.f=%%s|%%d\\n", buf, n);
  acc = acc + (unsigned)n;
  m = vf_%(u)s((int)(salt %% 50u), n, 1, buf);
  printf("%(u)s.v=%%d\\n", m);
  n = sprintf(buf, "%%02d:%%d", (int)(salt %% 60u), m);
  printf("%(u)s.g=%%s|%%d\\n", buf, n);
  vf_%(u)s(n, m, 9, buf);
  return acc + (unsigned)(m + n);
}
""" % {
        "u": u,
        "fmt": "".join(pieces),
        "args": ", ".join(args),
    }


def _render_stmt_expr(uid, p):
    u = "u%d" % uid
    return """#define MAXV_%(u)s(a, b) ({ int _a = (a); int _b = (b); _a > _b ? _a : _b; })
#define MINV_%(u)s(a, b) ({ int _p = (a); int _q = (b); _p < _q ? _p : _q; })
#define CLAMP_%(u)s(x, lo, hi) ({ int _x = (x); MAXV_%(u)s(MINV_%(u)s(_x, (hi)), (lo)); })
#define STEP_%(u)s(v) do { (v) = (v) + ({ int _d = (v) / 2; _d + 1; }); } while (0)

static int lbl_%(u)s(int n) {
  int r = ({
    int acc2 = 0;
  again_%(u)s:
    acc2 = acc2 + n;
    n = n - 1;
    if (n > 0)
      goto again_%(u)s;
    acc2;
  });
  return r;
}

static unsigned tmpl_se_%(u)s(unsigned salt) {
  int x = (int)(salt %% 9u) + %(xa)d;
  int y = (int)(salt %% 7u) + %(ya)d;
  int z;
  int j;
  for (j = 0; j < %(iters)d; j++) {
    x = x + MAXV_%(u)s(j * 3 %% 7, j);
    STEP_%(u)s(y);
    printf("%(u)s.j=%%d x=%%d y=%%d\\n", j, x, y);
  }
  z = CLAMP_%(u)s(x - y, %(lo)d, %(hi)d);
  printf("%(u)s.z=%%d min=%%d\\n", z, MINV_%(u)s(x, y));
  printf("%(u)s.l=%%d\\n", lbl_%(u)s((int)(salt %% 4u) + 2));
  return salt + (unsigned)(z + x + y + 40);
}
""" % {
        "u": u,
        "iters": p["iters"],
        "xa": p["xa"],
        "ya": p["ya"],
        "lo": p["lo"],
        "hi": p["hi"],
    }


def _render_devirt(uid, p):
    u = "u%d" % uid
    return """static int dsc_%(u)s(int x) { return x * %(mult)d + 1; }

int (*const dptr_%(u)s)(int) = &dsc_%(u)s;
int (*dfp_%(u)s)(FILE *, const char *, ...) = &fprintf;

static unsigned tmpl_devirt_%(u)s(unsigned salt) {
  int j;
  int a = 0;
  for (j = 0; j < %(iters)d; j++) {
    a = a + dptr_%(u)s(j + (int)(salt %% 5u));
    dfp_%(u)s(stdout, "%(u)s.j=%%d a=%%d\\n", j, a);
  }
  dfp_%(u)s(stdout, "%(u)s.t=%%d %%d\\n", a, (*dptr_%(u)s)(a %% 100));
  return salt + (unsigned)a;
}
""" % {
        "u": u,
        "mult": p["mult"],
        "iters": p["iters"],
    }


def _render_greturn(uid, p):
    u = "u%d" % uid
    return """struct GS_%(u)s { int m; int n; };
struct GS_%(u)s ggs_%(u)s = { %(m0)s, %(n0)s };
int gcl_%(u)s;

struct GS_%(u)s *ggo_%(u)s(void) {
  gcl_%(u)s++;
  return &ggs_%(u)s;
}

struct GS_%(u)s *gag_%(u)s(void) { return &ggs_%(u)s; }

static unsigned tmpl_gr_%(u)s(unsigned salt) {
  ggo_%(u)s()->m = (int)(salt %% 90u) + 1;
  printf("%(u)s.m=%%d c=%%d\\n", ggs_%(u)s.m, gcl_%(u)s);
  ggo_%(u)s()->m = ggo_%(u)s()->m + %(step)d;
  printf("%(u)s.m2=%%d c=%%d\\n", ggs_%(u)s.m, gcl_%(u)s);
  gag_%(u)s()->n = gag_%(u)s()->m + ggs_%(u)s.n %% 100;
  printf("%(u)s.n=%%d\\n", ggs_%(u)s.n);
  ggs_%(u)s.m = %(pool_small)s;
  printf("%(u)s.r=%%d %%d\\n", ggo_%(u)s()->m, gag_%(u)s()->n);
  return salt + (unsigned)(gcl_%(u)s * 3 + ggs_%(u)s.n %% 50 + 128);
}
""" % {
        "u": u,
        "m0": _int_lit(p["m0"]),
        "n0": _int_lit(p["n0"]),
        "step": p["step"],
        "pool_small": _int_lit(p["pool_small"]),
    }


def _render_carrier(uid, p):
    u = "u%d" % uid
    return """unsigned long bk_%(u)s;

static void *ext_%(u)s(unsigned long size, unsigned long align) {
  unsigned long mask = align - 1u;
  void *ret = 0;
  if (__builtin_expect(!!(bk_%(u)s == 0u), 0))
    return ret;
  bk_%(u)s = (bk_%(u)s + mask) & ~mask;
  ret = (void *)bk_%(u)s;
  bk_%(u)s += size;
  return ret;
}

static int isn_%(u)s(void *p) {
  if (p)
    return 0;
  return 1;
}

static int cls_%(u)s(unsigned long v) {
  if (__builtin_expect(v != 0u, 1))
    return 1;
  if (v == 0u)
    return 2;
}

static unsigned tmpl_cr_%(u)s(unsigned salt) {
  void *r;
  unsigned acc = salt;
  bk_%(u)s = 0u;
  r = ext_%(u)s(%(size)du, %(align)du);
  printf("%(u)s.a=%%d\\n", isn_%(u)s(r));
  bk_%(u)s = (unsigned long)(salt %% 512u) + %(base)du;
  r = ext_%(u)s(%(size2)du, %(align)du);
  if (!r)
    printf("%(u)s.b=null\\n");
  else
    printf("%(u)s.b=ok\\n");
  printf("%(u)s.c=%%d bk=%%lu\\n", isn_%(u)s(r), bk_%(u)s);
  printf("%(u)s.d=%%d %%d\\n", cls_%(u)s(salt %% 2u), cls_%(u)s((unsigned long)salt + 1u));
  return acc + (unsigned)(bk_%(u)s %% 1000u);
}
""" % {
        "u": u,
        "size": p["size"],
        "size2": p["size2"],
        "align": p["align"],
        "base": p["base"],
    }


def _render_writeback(uid, p):
    """Writeback-order shapes (ref: test/EndToEnd/globals-writeback-order.c).

    Every LHS store targets a global subobject while the RHS (or index
    expression) calls a helper that writes a provably DISJOINT subobject
    of the same global: the wide-byte window stays within [woff, woff+4)
    with woff <= 2 while the helpers write bytes 6 and 7; the struct
    helpers write the OTHER member; the index helpers return 0/1 while
    writing elements 2/3.  Function calls are indeterminately sequenced
    with the lvalue evaluation, so disjointness keeps every shape UB-free
    (C11 6.5.16p3).  Digests of BOTH subobjects print after each shape.
    """
    u = "u%d" % uid
    return """char gwb_%(u)s[8];
struct WS_%(u)s { int a; int b; };
struct WS_%(u)s gws_%(u)s;
struct WT_%(u)s { int x; int y; };
struct WT_%(u)s gwt_%(u)s;
int gwa_%(u)s[4];
unsigned wsl_%(u)s;

static unsigned wpk_%(u)s(void) {
  gwb_%(u)s[6] = (char)(40 + (int)(wsl_%(u)s %% 40u));
  return %(mask)s ^ (wsl_%(u)s %% 251u);
}

static unsigned wbp_%(u)s(void) {
  gwb_%(u)s[7] = (char)(30 + (int)(wsl_%(u)s %% 50u));
  return %(delta)s + (wsl_%(u)s %% 16u);
}

static int wtb_%(u)s(void) {
  gws_%(u)s.b = 70 + (int)(wsl_%(u)s %% 20u);
  return %(rv1)d + (int)(wsl_%(u)s %% 7u);
}

static int wtb2_%(u)s(void) {
  gws_%(u)s.b = 90 + (int)(wsl_%(u)s %% 9u);
  return %(rv2)s;
}

static struct WT_%(u)s *wgt_%(u)s(void) { return &gwt_%(u)s; }

static int wpy_%(u)s(void) {
  gwt_%(u)s.y = 80 + (int)(wsl_%(u)s %% 15u);
  return %(rv3)d + (int)(wsl_%(u)s %% 5u);
}

static int wi2_%(u)s(void) {
  gwa_%(u)s[2] = 33 + (int)(wsl_%(u)s %% 7u);
  return (int)(wsl_%(u)s %% 2u);
}

static int wi3_%(u)s(void) {
  gwa_%(u)s[3] = 44 + (int)(wsl_%(u)s %% 5u);
  return (int)((wsl_%(u)s / 3u) %% 2u);
}

static unsigned tmpl_wb_%(u)s(unsigned salt) {
  unsigned acc = salt;
  unsigned woff;
  int j;
  wsl_%(u)s = salt;
  woff = salt %% %(off_mod)du;
  for (j = 0; j < 8; j++)
    gwb_%(u)s[j] = (char)(j + 1);
  *(unsigned *)(gwb_%(u)s + woff) = wpk_%(u)s();
  printf("%(u)s.a w=%%u b6=%%u\\n", *(unsigned *)(gwb_%(u)s + woff) %% 100000u,
         (unsigned)gwb_%(u)s[6]);
  *(unsigned *)(gwb_%(u)s + woff) += wbp_%(u)s();
  printf("%(u)s.b w=%%u b6=%%u b7=%%u\\n", *(unsigned *)(gwb_%(u)s + woff) %% 100000u,
         (unsigned)gwb_%(u)s[6], (unsigned)gwb_%(u)s[7]);
  acc = acc * 31u + *(unsigned *)(gwb_%(u)s + woff);
  gws_%(u)s.a = wtb_%(u)s();
  printf("%(u)s.c a=%%d b=%%d\\n", gws_%(u)s.a, gws_%(u)s.b);
  gws_%(u)s.a += wtb2_%(u)s();
  printf("%(u)s.d a=%%d b=%%d\\n", gws_%(u)s.a, gws_%(u)s.b);
  wgt_%(u)s()->x = wpy_%(u)s();
  printf("%(u)s.e x=%%d y=%%d\\n", gwt_%(u)s.x, gwt_%(u)s.y);
  for (j = 0; j < 4; j++)
    gwa_%(u)s[j] = j * 2 + (int)(salt %% 5u);
  gwa_%(u)s[wi2_%(u)s()]++;
  printf("%(u)s.f1 %%d %%d %%d %%d\\n", gwa_%(u)s[0], gwa_%(u)s[1],
         gwa_%(u)s[2], gwa_%(u)s[3]);
  gwa_%(u)s[wi3_%(u)s()] += %(plus)d;
  printf("%(u)s.f2 %%d %%d %%d %%d\\n", gwa_%(u)s[0], gwa_%(u)s[1],
         gwa_%(u)s[2], gwa_%(u)s[3]);
  acc = acc + (unsigned)(gws_%(u)s.a + gws_%(u)s.b + gwt_%(u)s.x + gwt_%(u)s.y);
  acc = acc + (unsigned)(gwa_%(u)s[0] + gwa_%(u)s[1] + gwa_%(u)s[2] + gwa_%(u)s[3]);
  return acc;
}
""" % {
        "u": u,
        "mask": _uns_lit(p["mask"]),
        "delta": _uns_lit(p["delta"]),
        "rv1": p["rv1"],
        "rv2": _int_lit(p["rv2"]),
        "rv3": p["rv3"],
        "off_mod": p["off_mod"],
        "plus": p["plus"],
    }


# ---------------------------------------------------------------------------
# Template registry: name -> (parameter domains, renderer).  Domains are
# ordered lists; minimize.py shrinks toward the front of each list, and
# plan_program draws with rng.choice, so list order is the single source of
# both randomness and shrink direction.
# ---------------------------------------------------------------------------

TemplateSpec = namedtuple("TemplateSpec", ["name", "domains", "render", "fn_prefix"])

TEMPLATES = [
    TemplateSpec(
        "union_pun",
        {
            "iters": [1, 2, 3, 4],
            "mask": POOL_MASK,
            "pool_a": POOL_INT,
            "pool_b": POOL_INT,
            "negoff": [1, 7, 100],
        },
        _render_union_pun,
        "tmpl_union",
    ),
    TemplateSpec(
        "byte_pun",
        {
            "length": [12, 16],
            "use_global": [0, 1],
            "mask": POOL_MASK,
            "delta": [1, 0x55AA, 0xAA55, 0x7FFFFFFF],
        },
        _render_byte_pun,
        "tmpl_bytes",
    ),
    TemplateSpec(
        "cell_slice",
        {"perm": [0, 1, 2], "extra": [1, 2, 3]},
        _render_cell_slice,
        "tmpl_cells",
    ),
    TemplateSpec(
        "ptr_mix",
        {
            "iters": [1, 3, 4, 5],
            "base": [0, 5, 17, 64],
            "pool_small": POOL_SMALL,
            "thresh": [0, 10, 39],
        },
        _render_ptr_mix,
        "tmpl_ptrs",
    ),
    TemplateSpec(
        "va_sprintf",
        {
            "fmt_a": list(range(len(_FMT_MATRIX))),
            "fmt_b": list(range(len(_FMT_MATRIX))),
            "fmt_c": list(range(len(_FMT_MATRIX))),
            "val_a": POOL_INT,
            "val_b": POOL_INT,
            "val_c": POOL_INT,
        },
        _render_va_sprintf,
        "tmpl_va",
    ),
    TemplateSpec(
        "stmt_expr",
        {
            "iters": [1, 3, 5, 6],
            "xa": [0, 4, 9],
            "ya": [1, 3, 9],
            "lo": [0, 3, -5],
            "hi": [20, 50, 9],
        },
        _render_stmt_expr,
        "tmpl_se",
    ),
    TemplateSpec(
        "devirt",
        {"iters": [1, 3, 5, 6], "mult": [2, 3, 5]},
        _render_devirt,
        "tmpl_devirt",
    ),
    TemplateSpec(
        "greturn",
        {
            "m0": POOL_SMALL,
            "n0": [4, 0, 42],
            "step": [1, 2, 13],
            "pool_small": POOL_SMALL,
        },
        _render_greturn,
        "tmpl_gr",
    ),
    TemplateSpec(
        "carrier",
        {
            "size": [64, 8, 4096],
            "size2": [4096, 16, 64],
            "align": [16, 8, 32],
            "base": [1024, 8, 65536],
        },
        _render_carrier,
        "tmpl_cr",
    ),
    TemplateSpec(
        "writeback_order",
        {
            "off_mod": [1, 2, 3],
            "mask": POOL_MASK,
            "delta": [1, 0x10, 0x55AA, 0x7FFFFFFF],
            "rv1": [5, 1, 9],
            "rv2": [9, 2, -3],
            "rv3": [13, 1, 7],
            "plus": [5, 1, 3],
        },
        _render_writeback,
        "tmpl_wb",
    ),
]

_TEMPLATES_BY_NAME = {spec.name: spec for spec in TEMPLATES}

# Templates that exercise pointer provenance; the cross-feature composer
# forces at least two of these into a CROSS_FRACTION of the seeds.
PROVENANCE_TEMPLATES = [
    "byte_pun",
    "cell_slice",
    "ptr_mix",
    "devirt",
    "greturn",
    "carrier",
    "writeback_order",
]

# The tmpl_* call target per template, used by main rendering.
_FN_PREFIX = {spec.name: spec.fn_prefix for spec in TEMPLATES}


# ---------------------------------------------------------------------------
# Expected-output oracle: an exact Python evaluator per template.  Each
# evaluator mirrors its renderer statement by statement with explicit
# wrapping arithmetic (32-bit two's-complement int/unsigned, 64-bit for the
# carrier's unsigned long, little-endian byte puns per the module-level
# byteorder assertion) and appends the same printf lines the C program
# emits.  evaluate_plan() returns the exact (stdout bytes, exit code) both
# compiled legs must reproduce; any native divergence from it is a
# GENERATOR_ORACLE_BUG in differ.py.  No template is oracle-exempt.
# ---------------------------------------------------------------------------

_M32 = 0xFFFFFFFF
_M64 = 0xFFFFFFFFFFFFFFFF


def _u32(value):
    """Wrap to C unsigned int (32-bit)."""
    return value & _M32


def _i32(value):
    """Reinterpret the low 32 bits as C int (two's complement)."""
    value &= _M32
    return value - 0x100000000 if value & 0x80000000 else value


def _u64(value):
    """Wrap to C unsigned long (64-bit on the LP64 host)."""
    return value & _M64


def _ld32(buf, off):
    """Little-endian u32 load from a bytearray (the *(unsigned *) view)."""
    return buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)


def _st32(buf, off, value):
    """Little-endian u32 store into a bytearray (the *(unsigned *) view)."""
    value = _u32(value)
    buf[off] = value & 0xFF
    buf[off + 1] = (value >> 8) & 0xFF
    buf[off + 2] = (value >> 16) & 0xFF
    buf[off + 3] = (value >> 24) & 0xFF


def _eval_union_pun(uid, p, salt, out):
    u = "u%d" % uid
    acc = salt
    for j in range(p["iters"]):
        vu = _u32(acc ^ p["mask"])
        out.append("%s.a=%d\n" % (u, _i32(vu)))
        vu = _u32(p["pool_a"]) ^ (salt % 13)
        acc = _u32(acc + vu + j)
        out.append("%s.b=%d acc=%d\n" % (u, _i32(vu), acc % 100000))
    vu = _u32(-(salt % 100) - p["negoff"])
    out.append("%s.n=%d\n" % (u, vu))
    acc = _u32(acc + vu)
    tag = salt % 17
    hpu = _u32(p["pool_b"]) ^ (salt % 9)
    out.append("%s.h=%d %d %d\n" % (u, tag, _i32(hpu), hpu))
    return _u32(acc + hpu + tag)


def _eval_byte_pun(uid, p, salt, out):
    u = "u%d" % uid
    length = p["length"]
    acc = salt
    arr = bytearray(length)
    for j in range(length):
        arr[j] = ord("A") + (j + salt % 7) % 26
    out.append("%s.i j=%d\n" % (u, salt % 7))
    for j in range(3):
        off = (j * (length - 4)) // 2
        w = _ld32(arr, off)
        acc = _u32(acc * 31 + (w % 65536))
        _st32(arr, off, w ^ p["mask"])
        out.append("%s.w j=%d off=%d w=%d\n" % (u, j, off, w % 100000))
    off = salt % (length - 3)
    _st32(arr, off, _ld32(arr, off) + _u32(p["delta"] ^ (salt | 1)))
    w = _ld32(arr, off)
    out.append("%s.c off=%d w=%d\n" % (u, off, w % 100000))
    acc = _u32(acc + w)
    for j in range(length - 1):
        arr[j] = ord("a") + arr[j] % 26
    out.append("%s.s=%s\n" % (u, arr[: length - 1].decode("ascii")))
    return acc


def _eval_cell_slice(uid, p, salt, out):
    u = "u%d" % uid
    arr_a = [j + 1 + salt % 3 for j in range(4)]
    arr_b = [0] * 4
    arr_c = [0] * 4

    def cpr():
        out.append(
            "%s.st%s |%s |%s\n"
            % (
                u,
                "".join(" %d" % v for v in arr_a),
                "".join(" %d" % v for v in arr_b),
                "".join(" %d" % v for v in arr_c),
            )
        )

    def cmv(s, d):
        i = 0
        while i < 4 and s[i] == 0:
            i += 1
        j = 0
        while j < 4 and d[j] == 0:
            j += 1
        if i >= 4 or j <= 0:
            out.append("%s.skip\n" % u)
            return 0
        d[j - 1] = s[i]
        s[i] = 0
        cpr()
        return d[j - 1]

    perm1, perm2 = _CELL_PERMS[p["perm"]]

    def chn(n, s, d, sp):
        if n <= 1:
            cmv(s, d)
            return
        env = {"s": s, "d": d, "sp": sp}
        chn(n - 1, *[env[t.strip()] for t in perm1.split(",")])
        cmv(s, d)
        chn(n - 1, *[env[t.strip()] for t in perm2.split(",")])

    n = arr_a[1] - arr_a[0] + p["extra"]
    out.append("%s.n=%d\n" % (u, n))
    cpr()
    m = cmv(arr_a, arr_c)
    out.append("%s.m=%d\n" % (u, m))
    m += cmv(arr_c, arr_a)
    chn(n, arr_a, arr_b, arr_c)
    cpr()
    return _u32(salt * 3 + (arr_a[0] + arr_b[3] + arr_c[3] + m + 64))


def _eval_ptr_mix(uid, p, salt, out):
    u = "u%d" % uid
    acc = salt
    x = salt % 100 + p["base"]
    y = salt % 30 + 1
    for i in range(p["iters"]):
        x += i
        acc = _u32(acc + _u32(x))
        if acc % 2 == 0:
            x += 1
            out.append("%s.q=%d\n" % (u, x))
        else:
            out.append("%s.null i=%d\n" % (u, i))
    x += acc % 50
    out.append("%s.x=%d\n" % (u, x))
    a = salt % 40
    b = p["pool_small"]
    if a > p["thresh"]:
        b += a
    else:
        y += a
    out.append("%s.mb=%d %d %d y=%d\n" % (u, a, b, 3, y))
    acc = _u32(acc + _u32(y))
    uu = _u32(_u32(x) * 2654435761)
    acc = _u32(acc + uu)
    out.append("%s.u=%d\n" % (u, uu % 100000))
    return _u32(acc + _u32(b))


def _sprintf_piece(index, uid_tag, lit, salt):
    """Format one _FMT_MATRIX slot exactly as C sprintf would."""
    if index == 0:
        return "[%d]" % _i32(_u32(lit) ^ (salt % 31))
    if index == 1:
        return "[%5d]" % (-(salt % 90) - 7)
    if index == 2:
        return "[%-6d]" % (salt % 1000 - 500)
    if index == 3:
        return "[%05d]" % (-(salt % 500))
    if index == 4:
        return "[%d]" % _u32(_u32(lit) + salt % 65536)
    if index == 5:
        return "[%x]" % (_u32(lit) | (salt % 255))
    if index == 6:
        return "[%08x]" % _u32(_u32(lit) ^ (salt % 4096))
    if index == 7:
        return "[%02d]" % (salt % 7)
    if index == 8:
        return "[%c]" % chr(ord("a") + salt % 26)
    return "[s%s]" % uid_tag


def _eval_va_sprintf(uid, p, salt, out):
    u = "u%d" % uid
    acc = salt
    buf = "".join(
        _sprintf_piece(p[slot], u, p["val_" + slot[-1]], salt)
        for slot in ("fmt_a", "fmt_b", "fmt_c")
    )
    n = len(buf)
    out.append("%s.f=%s|%d\n" % (u, buf, n))
    acc = _u32(acc + n)
    a = salt % 50
    m = a - n if a > n else n - a + 1
    out.append("%s.v=%d\n" % (u, m))
    buf2 = "%02d:%d" % (salt % 60, m)
    n2 = len(buf2)
    out.append("%s.g=%s|%d\n" % (u, buf2, n2))
    return _u32(acc + _u32(m + n2))


def _eval_stmt_expr(uid, p, salt, out):
    u = "u%d" % uid
    x = salt % 9 + p["xa"]
    y = salt % 7 + p["ya"]
    for j in range(p["iters"]):
        x += max(j * 3 % 7, j)
        y += y // 2 + 1
        out.append("%s.j=%d x=%d y=%d\n" % (u, j, x, y))
    z = max(min(x - y, p["hi"]), p["lo"])
    out.append("%s.z=%d min=%d\n" % (u, z, min(x, y)))
    n0 = salt % 4 + 2
    out.append("%s.l=%d\n" % (u, n0 * (n0 + 1) // 2))
    return _u32(salt + _u32(z + x + y + 40))


def _eval_devirt(uid, p, salt, out):
    u = "u%d" % uid
    a = 0
    for j in range(p["iters"]):
        a += (j + salt % 5) * p["mult"] + 1
        out.append("%s.j=%d a=%d\n" % (u, j, a))
    out.append("%s.t=%d %d\n" % (u, a, (a % 100) * p["mult"] + 1))
    return _u32(salt + a)


def _eval_greturn(uid, p, salt, out):
    u = "u%d" % uid
    n = p["n0"]
    m = salt % 90 + 1
    gcl = 1
    out.append("%s.m=%d c=%d\n" % (u, m, gcl))
    m += p["step"]
    gcl += 2
    out.append("%s.m2=%d c=%d\n" % (u, m, gcl))
    n = m + n % 100
    out.append("%s.n=%d\n" % (u, n))
    m = p["pool_small"]
    gcl += 1  # ggo() inside the .r printf line.
    out.append("%s.r=%d %d\n" % (u, m, n))
    return _u32(salt + _u32(gcl * 3 + n % 50 + 128))


def _eval_carrier(uid, p, salt, out):
    u = "u%d" % uid
    # bk starts 0, so the first ext() returns NULL: isn() prints 1.
    out.append("%s.a=%d\n" % (u, 1))
    mask = p["align"] - 1
    bk = (salt % 512) + p["base"]
    bk = _u64((bk + mask) & ~mask)
    # r = bk (nonzero since base >= 8); then bk advances by size2.
    bk = _u64(bk + p["size2"])
    out.append("%s.b=ok\n" % u)
    out.append("%s.c=%d bk=%d\n" % (u, 0, bk))
    out.append("%s.d=%d %d\n" % (u, 1 if salt % 2 != 0 else 2, 1))
    return _u32(salt + bk % 1000)


def _eval_writeback(uid, p, salt, out):
    u = "u%d" % uid
    acc = salt
    woff = salt % p["off_mod"]
    gwb = bytearray(range(1, 9))
    gwb[6] = 40 + salt % 40  # wpk()'s disjoint byte write.
    _st32(gwb, woff, p["mask"] ^ (salt % 251))
    out.append("%s.a w=%d b6=%d\n" % (u, _ld32(gwb, woff) % 100000, gwb[6]))
    gwb[7] = 30 + salt % 50  # wbp()'s disjoint byte write.
    _st32(gwb, woff, _ld32(gwb, woff) + _u32(p["delta"] + salt % 16))
    out.append(
        "%s.b w=%d b6=%d b7=%d\n" % (u, _ld32(gwb, woff) % 100000, gwb[6], gwb[7])
    )
    acc = _u32(acc * 31 + _ld32(gwb, woff))
    b = 70 + salt % 20
    a = p["rv1"] + salt % 7
    out.append("%s.c a=%d b=%d\n" % (u, a, b))
    b = 90 + salt % 9
    a += p["rv2"]
    out.append("%s.d a=%d b=%d\n" % (u, a, b))
    y = 80 + salt % 15
    x = p["rv3"] + salt % 5
    out.append("%s.e x=%d y=%d\n" % (u, x, y))
    gwa = [j * 2 + salt % 5 for j in range(4)]
    gwa[2] = 33 + salt % 7  # wi2()'s element write, then the ++ writeback.
    gwa[salt % 2] += 1
    out.append("%s.f1 %d %d %d %d\n" % (u, gwa[0], gwa[1], gwa[2], gwa[3]))
    gwa[3] = 44 + salt % 5  # wi3()'s element write, then the += writeback.
    gwa[(salt // 3) % 2] += p["plus"]
    out.append("%s.f2 %d %d %d %d\n" % (u, gwa[0], gwa[1], gwa[2], gwa[3]))
    acc = _u32(acc + _u32(a + b + x + y))
    return _u32(acc + _u32(sum(gwa)))


_EVALUATORS = {
    "union_pun": _eval_union_pun,
    "byte_pun": _eval_byte_pun,
    "cell_slice": _eval_cell_slice,
    "ptr_mix": _eval_ptr_mix,
    "va_sprintf": _eval_va_sprintf,
    "stmt_expr": _eval_stmt_expr,
    "devirt": _eval_devirt,
    "greturn": _eval_greturn,
    "carrier": _eval_carrier,
    "writeback_order": _eval_writeback,
}


def evaluate_plan(plan):
    """Exact expected behavior of a plan's program.

    Returns ``(stdout_bytes, exit_code)``: the same digest lines and the
    same final ``acc % 251`` the rendered C program produces on both
    differential legs.  Pure function of the plan; consumes no randomness.
    """
    out = []
    acc = plan.seed % 65536
    for gi in range(plan.warm):
        acc = _u32(acc * 1103515245 + gi + plan.seed_mix)
    out.append("boot=%d\n" % (acc % 99991))
    for inst in plan.instances:
        acc = _EVALUATORS[inst.name](inst.uid, inst.params, acc, out)
        out.append("dg%d=%d\n" % (inst.uid, acc % 65521))
    out.append("final=%d\n" % (acc % 100000))
    return "".join(out).encode("ascii"), acc % 251


def plan_program(seed, cross_fraction=CROSS_FRACTION):
    """Build the deterministic Plan for ``seed``.

    All randomness flows through one ``random.Random(seed)`` and every
    choice draws from an explicitly ordered list, so the plan (and hence
    the rendered program) is a pure function of the seed.
    """
    rng = random.Random(seed)
    count = rng.choice([2, 3, 3, 4, 4, 5])
    names = []
    if rng.random() < cross_fraction:
        names.extend(rng.sample(PROVENANCE_TEMPLATES, 2))
    remaining = [spec.name for spec in TEMPLATES if spec.name not in names]
    while len(names) < count:
        pick = rng.choice(remaining)
        names.append(pick)
        remaining.remove(pick)
    rng.shuffle(names)
    instances = []
    for uid, name in enumerate(names):
        spec = _TEMPLATES_BY_NAME[name]
        params = {}
        for key in sorted(spec.domains):
            params[key] = rng.choice(spec.domains[key])
        instances.append(Instance(uid=uid, name=name, params=params))
    warm = rng.choice([3, 4, 5, 6, 7])
    seed_mix = rng.randrange(0, 1 << 31)
    return Plan(seed=seed, warm=warm, seed_mix=seed_mix, instances=instances)


def render_plan(plan):
    """Render a Plan to complete C11 source text (pure function of the plan)."""
    tops = []
    calls = []
    for inst in plan.instances:
        spec = _TEMPLATES_BY_NAME[inst.name]
        tops.append(spec.render(inst.uid, inst.params))
        calls.append("  acc = %s_u%d(acc);" % (spec.fn_prefix, inst.uid))
        calls.append('  printf("dg%d=%%u\\n", acc %% 65521u);' % inst.uid)
    header = (
        "/* Generated by test/Fuzz/genprog.py version %s, seed %d.\n"
        "   Templates: %s. Deterministic: same seed -> identical bytes. */\n"
        "#include <stdio.h>\n\n" % (
            GENERATOR_VERSION,
            plan.seed,
            ", ".join(i.name for i in plan.instances),
        )
    )
    main_fn = (
        "int main(void) {\n"
        "  unsigned acc = %du;\n"
        "  int gi;\n"
        "  for (gi = 0; gi < %d; gi++)\n"
        "    acc = acc * 1103515245u + (unsigned)gi + %du;\n"
        '  printf("boot=%%u\\n", acc %% 99991u);\n'
        "%s\n"
        '  printf("final=%%u\\n", acc %% 100000u);\n'
        "  return (int)(acc %% 251u);\n"
        "}\n" % (plan.seed % 65536, plan.warm, plan.seed_mix, "\n".join(calls))
    )
    return header + "\n".join(tops) + "\n" + main_fn


def generate_program(seed, cross_fraction=CROSS_FRACTION):
    """Generate the C program for ``seed``; returns a Program namedtuple.

    ``expected_stdout``/``expected_exit`` come from the exact evaluator
    (evaluate_plan) and are what BOTH compiled legs must reproduce.
    """
    plan = plan_program(seed, cross_fraction)
    expected_stdout, expected_exit = evaluate_plan(plan)
    return Program(
        source=render_plan(plan),
        template_names=[inst.name for inst in plan.instances],
        plan=plan,
        expected_stdout=expected_stdout,
        expected_exit=expected_exit,
    )


def main(argv):
    """CLI: print (or write) the program for one seed."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("-o", "--output", default=None, help="Write to file instead of stdout.")
    parser.add_argument(
        "--cross-fraction",
        type=float,
        default=CROSS_FRACTION,
        help="Fraction of seeds forced to combine >=2 pointer-provenance templates.",
    )
    parser.add_argument(
        "--print-templates",
        action="store_true",
        help="Print the template names for the seed on stderr.",
    )
    parser.add_argument(
        "--expected",
        default=None,
        help="Also write the oracle's expected stdout bytes to this path"
        " (the expected exit code is printed on stderr).",
    )
    args = parser.parse_args(argv)
    program = generate_program(args.seed, args.cross_fraction)
    if args.print_templates:
        sys.stderr.write(" ".join(program.template_names) + "\n")
    if args.expected:
        with open(args.expected, "wb") as handle:
            handle.write(program.expected_stdout)
        sys.stderr.write("expected-exit %d\n" % program.expected_exit)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(program.source)
    else:
        sys.stdout.write(program.source)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
