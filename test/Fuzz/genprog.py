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

Templates mirror the executable spec in test/EndToEnd/*.c: union puns
(unions.c), byte reinterprets over char arrays (pointers-reinterpret.c),
cell-slice globals with permuted recursion (pointers-global-args.c),
void*/member-base/null-ternary pointers (pointers-void.c,
pointers-member-base.c, pointers-null-ternary.c), variadic
extra-dropping + sprintf formats (varargs-def.c, sprintf.c), statement
expressions (stmt-expr.c), fn-ptr devirtualization (fnptr-devirt.c),
global-return chains (pointers-return-global.c), and int-carrier +
__builtin_expect + missing-return combos
(missing-return-expect-carrier.c).  A configurable fraction of seeds
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
GENERATOR_VERSION = "1"

# Fraction of seeds forced to combine >= 2 pointer-provenance templates.
CROSS_FRACTION = 0.5

# Width-extreme / sparse / negative constants threaded into templates.
# INT_MIN itself is excluded (negating it is UB); INT_MIN+1 stands in.
POOL_INT = [-1, 0, 1, -2147483647, 2147483647, 0x55AA, 0xAA55, -21847]
POOL_SMALL = [-1, 0, 1, 7, 42, -9]
POOL_MASK = [0x55AA55AA, 0xAA55AA55, 0x01010101, 0x7FFFFFFF, 0x00FF00FF]

Instance = namedtuple("Instance", ["uid", "name", "params"])
Plan = namedtuple("Plan", ["seed", "warm", "seed_mix", "instances"])
Program = namedtuple("Program", ["source", "template_names", "plan"])


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
]

# The tmpl_* call target per template, used by main rendering.
_FN_PREFIX = {spec.name: spec.fn_prefix for spec in TEMPLATES}


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
    """Generate the C program for ``seed``; returns a Program namedtuple."""
    plan = plan_program(seed, cross_fraction)
    return Program(
        source=render_plan(plan),
        template_names=[inst.name for inst in plan.instances],
        plan=plan,
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
    args = parser.parse_args(argv)
    program = generate_program(args.seed, args.cross_fraction)
    if args.print_templates:
        sys.stderr.write(" ".join(program.template_names) + "\n")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(program.source)
    else:
        sys.stdout.write(program.source)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
