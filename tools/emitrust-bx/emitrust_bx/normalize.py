"""The FR-254 normalizer: source-to-source rewrite rules that move C++
outside the subset INTO it (opt-in, ``--normalize``).

A rule is ``Rule(name, kind, guard, apply)`` over clang's JSON AST:

* ``kind``   -- the clang node kind it matches (the PATTERN);
* ``guard``  -- ``guard(n) -> None | reason``: None when the rewrite is
  sound for THIS node, else the located refusal's text (the importer
  reports it at the node);
* ``apply``  -- ``apply(n) -> [stmt node, ...]``: the rewritten C++, as
  clang JSON statement nodes the importer then imports exactly like
  hand-written source (so every later analysis -- typecheck, FR-263
  sequencing, FR-261 pointers -- sees the rewritten program, not a
  special case).

Synthetic nodes reuse the ranges of the nodes they replace, so a residual
inside a rewritten loop is still located in the user's source.  The
rewritten program is canonical subset C++: ``--normalize --emit=cpp``
prints it, and that text re-imports WITHOUT ``--normalize`` to the same
Kernel (cli.check_roundtrip).

Rules (each validated by tests/test_fr254.py: unit cases, a seeded
generator byte-diffed natively before/after, and test/EndToEnd):

R1 for->while  ``for (I; C; S) B`` -> ``{ I; while (C) { B S; } }``
   (``C`` absent -> ``true``; a non-declaration ``I`` needs no block).
   Applied only when the canonical counting form does not match.
   Guards: no ``continue`` bound to this loop (C++ runs ``S`` on
   ``continue``; the while form would skip it); no condition variable.
   ``break`` and ``return`` need none: both forms skip ``S``.  A ``S``
   mentioning a name ``B`` declares at its top level would be CAPTURED by
   the splice, so the body is kept as a nested block: ``{ B } S;``.
R2 do-while    ``do B while (C);`` -> ``while (true) { B if (!(C)) { break; } }``.
   Guard: no ``continue`` bound to this loop (C++ jumps to the test
   ``C``; the while form would skip it).  ``C`` mentioning a name ``B``
   declares at its top level keeps ``{ B }`` nested, as in R1.
R3 post-incdec  an expression statement, a single-variable declaration or a
   ``return`` whose expression contains exactly ONE increment/decrement used
   as a value, and that one POSTFIX on a local ``x`` or on ``*p`` (``p`` a
   local pointer) that the subset refuses (a local integer ``i++`` is
   already a Kernel IncDec): the expression reads the OLD value in place and
   a separate prefix ``++x;`` / ``--x;`` follows it.  ``return E;`` becomes
   ``const T t = E; ++x; return t;`` with a fresh ``t``.  Sound because the
   only difference is WHEN the side effect lands -- at the end of the
   statement instead of somewhere inside it -- and the guards make that
   unobservable:
   * nothing else in the statement names ``x`` / ``p`` (by NAME,
     conservative: FR-259's own rule and wording, the importer's
     ``TC.INCDEC_ALSO_USES``).  This is the only path to the object inside
     the subset: no globals, and two Rust borrows of one object (an alias
     through another pointer) are refused by rustc, never run;
   * the statement calls no closure or function through a variable (a
     by-reference capture reaches ``x`` without naming it, and C++ leaves
     such a call indeterminately sequenced with the side effect);
   * the site is not under ``&&``, ``||``, ``?:`` or ``,`` (it might not
     be evaluated at all; conservative even on the always-evaluated side).
   A prefix ``++x`` used as a value and a statement with two or more
   increments keep the importer's own refusal.  NOT a hazard (the FR-254
   lead suspected one): in ``a[(*p)++] = f()`` C++17 sequences ``f()``
   before the whole left side, so ``a[*p] = f(); ++*p;`` reads ``*p`` at
   the same point (and Rust evaluates an assignment's value first, too).
"""

from dataclasses import dataclass
from typing import Callable, List, Optional

from . import typecheck as TC

LOOPS = ("ForStmt", "WhileStmt", "DoStmt", "CXXForRangeStmt")
# a `continue` inside these cannot be bound to an enclosing loop
OPAQUE = ("LambdaExpr", "CXXRecordDecl", "FunctionDecl", "BlockExpr")

FOR_CONTINUE = ("`for` loop with a `continue` bound to it (normalize for->while: the `while` form's "
                "`continue` would skip the step)")
FOR_CVAR = "`for` loop with a condition variable (normalize for->while: no rule)"
DO_CONTINUE = ("do-while loop with a `continue` bound to it (normalize do-while: the `while (true)` form's "
               "`continue` would skip the condition)")


# R3: located at the increment
INC_OPERAND = ("increment/decrement used as a value on something other than a local variable or `*p` "
               "(normalize post-incdec: no rule)")
INC_COND = ("increment/decrement used as a value inside `&&`, `||`, `?:` or `,` (normalize post-incdec: it "
            "might not be evaluated)")
INC_CLOSURE = ("increment/decrement of '%s' used as a value in a statement that calls a closure or a function "
               "through a variable (normalize post-incdec: the call may read or modify '%s')")
INC_RETURN = ("increment/decrement used as a value in a `return` of type '%s' (normalize post-incdec: the "
              "temporary must be a scalar)")
# `x && y`, `x || y`, `x, y`: an operand not evaluated, or a sequence point
COND_OPS = ("&&", "||", ",")
COND_KINDS = ("ConditionalOperator", "BinaryConditionalOperator")


@dataclass(frozen=True)
class Rule:
    name: str
    kind: str
    guard: Callable[[dict], Optional[str]]
    apply: Callable[[dict], List[dict]]


# ------------------------------------------------------------------ queries

def _kids(n):
    for k, v in n.items():
        if k in ("loc", "range", "type", "referencedDecl", "foundReferencedDecl"):
            continue
        if isinstance(v, dict):
            yield v
        elif isinstance(v, list):
            for x in v:
                if isinstance(x, dict):
                    yield x


def bound_continue(body):
    """The first `continue` in ``body`` bound to the loop that owns
    ``body`` (not to a nested loop), or None."""
    stack = [body]
    while stack:
        x = stack.pop()
        if not isinstance(x, dict):
            continue
        k = x.get("kind")
        if k == "ContinueStmt":
            return x
        if k in LOOPS or k in OPAQUE:
            continue
        stack.extend(_kids(x))
    return None


def body_list(b):
    """``b``'s statements: a CompoundStmt's own, or ``[b]``."""
    if not b:
        return []
    if b.get("kind") == "CompoundStmt":
        return list(b.get("inner", []))
    return [b]


def top_names(stmts):
    """Names declared at the top level of a statement list."""
    out = set()
    for s in stmts:
        if isinstance(s, dict) and s.get("kind") == "DeclStmt":
            for d in s.get("inner", []):
                if d.get("name"):
                    out.add(d["name"])
    return out


def mentions(n, names):
    """Does ``n`` refer (by NAME -- conservative) to any of ``names``?"""
    if not names or not n:
        return False
    stack = [n]
    while stack:
        x = stack.pop()
        if not isinstance(x, dict):
            continue
        if x.get("kind") == "DeclRefExpr" and x.get("referencedDecl", {}).get("name") in names:
            return True
        stack.extend(_kids(x))
    return False


# ------------------------------------------------------------------ builders

def _compound(rng, stmts):
    return {"kind": "CompoundStmt", "range": rng, "inner": list(stmts)}


def _true(rng):
    return {"kind": "CXXBoolLiteralExpr", "range": rng, "type": {"qualType": "bool"},
            "valueCategory": "prvalue", "value": True}


def _not(c):
    rng = c.get("range")
    paren = {"kind": "ParenExpr", "range": rng, "type": {"qualType": "bool"}, "valueCategory": "prvalue",
             "inner": [c]}
    return {"kind": "UnaryOperator", "range": rng, "type": {"qualType": "bool"}, "valueCategory": "prvalue",
            "isPostfix": False, "opcode": "!", "inner": [paren]}


def _while(rng, cond, body):
    return {"kind": "WhileStmt", "range": rng, "inner": [cond, body]}


def _spliced(body, tail_expr, tail_stmts):
    """``B`` followed by ``tail_stmts``: B's statements spliced in, or B
    kept as a nested block when ``tail_expr`` mentions a name B declares at
    its top level (splicing would capture it)."""
    bs = body_list(body)
    if mentions(tail_expr, top_names(bs)):
        bs = [_compound(body.get("range"), bs)]
    return bs + tail_stmts


# ------------------------------------------------------------------ R1

def _for_parts(n):
    inner = n.get("inner", [])
    if len(inner) != 5:
        return None
    return inner


def for_guard(n):
    parts = _for_parts(n)
    if parts is None:
        return FOR_CVAR
    init, cvar, cond, inc, body = parts
    if cvar:
        return FOR_CVAR
    if bound_continue(body) is not None:
        return FOR_CONTINUE
    return None


def for_apply(n):
    init, _, cond, inc, body = _for_parts(n)
    rng = n.get("range")
    wbody = _compound(body.get("range") if body else rng, _spliced(body, inc, [inc] if inc else []))
    w = _while(rng, cond if cond else _true(rng), wbody)
    if init and init.get("kind") == "DeclStmt":
        return [_compound(rng, [init, w])]  # I's names stay scoped to the loop
    return ([init] if init else []) + [w]


# ------------------------------------------------------------------ R2

def do_guard(n):
    body = (n.get("inner") or [None])[0]
    if bound_continue(body) is not None:
        return DO_CONTINUE
    return None


def do_apply(n):
    body, cond = n["inner"][0], n["inner"][1]
    rng = n.get("range")
    crng = cond.get("range")
    test = {"kind": "IfStmt", "range": crng, "inner": [_not(cond), _compound(crng, [{"kind": "BreakStmt",
                                                                                     "range": crng}])]}
    return [_while(rng, _true(rng), _compound(body.get("range"), _spliced(body, cond, [test])))]


# ------------------------------------------------------------------ R3

def is_incdec(n):
    return isinstance(n, dict) and n.get("kind") == "UnaryOperator" and n.get("opcode") in ("++", "--")


def strip_value(n):
    """``n`` without parentheses and value/no-op conversions."""
    while isinstance(n, dict) and (n.get("kind") in ("ParenExpr", "ExprWithCleanups", "ConstantExpr")
                                   or (n.get("kind") == "ImplicitCastExpr"
                                       and n.get("castKind") in ("LValueToRValue", "NoOp"))):
        n = n["inner"][0]
    return n


def incdec_sites(e):
    """[(increment node, conditional)] in ``e`` (not inside a lambda body or
    a local class); ``conditional``: under ``&&`` / ``||`` / ``?:`` / ``,``."""
    out = []
    stack = [(e, False)]
    while stack:
        x, c = stack.pop()
        if not isinstance(x, dict):
            continue
        k = x.get("kind")
        if k in OPAQUE:
            continue
        if is_incdec(x):
            out.append((x, c))
        cc = c or k in COND_KINDS or (k == "BinaryOperator" and x.get("opcode") in COND_OPS)
        stack.extend((y, cc) for y in _kids(x))
    return out


def incdec_root(u):
    """The DeclRefExpr of ``x`` in ``x++`` or of ``p`` in ``(*p)++``, or None."""
    o = strip_value(u["inner"][0])
    if isinstance(o, dict) and o.get("kind") == "UnaryOperator" and o.get("opcode") == "*":
        o = strip_value(o["inner"][0])
    if isinstance(o, dict) and o.get("kind") == "DeclRefExpr":
        return o
    return None


def other_mentions(n, name, skip):
    """Does ``n`` name ``name`` outside the subtree ``skip`` (a declaration
    of ``name`` counts)?"""
    stack = [n]
    while stack:
        x = stack.pop()
        if not isinstance(x, dict) or x is skip:
            continue
        if x.get("kind") == "DeclRefExpr" and x.get("referencedDecl", {}).get("name") == name:
            return True
        if x.get("kind") == "VarDecl" and x.get("name") == name:
            return True
        stack.extend(_kids(x))
    return False


def indirect_call(n):
    """The first call in ``n`` that is not a direct call of a named function
    (a closure, a lambda, a function pointer or a callable parameter), or
    None.  Looks inside lambda bodies too: a lambda in the statement is
    refused here whether or not it is called."""
    stack = [n]
    while stack:
        x = stack.pop()
        if not isinstance(x, dict):
            continue
        k = x.get("kind")
        if k == "LambdaExpr":
            return x
        if k == "CXXOperatorCallExpr" and x.get("inner"):
            callee = x["inner"][0]
            while isinstance(callee, dict) and callee.get("kind") in ("ImplicitCastExpr", "ParenExpr"):
                callee = callee["inner"][0]
            if not isinstance(callee, dict) or callee.get("referencedDecl", {}).get("name") == "operator()":
                return x
        if k == "CallExpr" and x.get("inner"):
            callee = x["inner"][0]
            while isinstance(callee, dict) and callee.get("kind") in ("ImplicitCastExpr", "ParenExpr"):
                if callee.get("kind") == "ImplicitCastExpr" and callee.get("castKind") != "FunctionToPointerDecay":
                    return x  # a pointer variable read (LValueToRValue) or a conversion
                callee = callee["inner"][0]
            if not (isinstance(callee, dict) and callee.get("kind") == "DeclRefExpr"
                    and callee.get("referencedDecl", {}).get("kind") in ("FunctionDecl", "CXXMethodDecl")):
                return x
        stack.extend(_kids(x))
    return None


def incdec_guard(n, u, conditional):
    """None when R3 may rewrite statement ``n`` around its one increment
    ``u``, else the refusal (to be located at ``u``).  The root's
    locality and the return type are the importer's checks."""
    root = incdec_root(u)
    if root is None:
        return INC_OPERAND
    if conditional:
        return INC_COND
    name = root["referencedDecl"].get("name")
    if indirect_call(n) is not None:
        return INC_CLOSURE % (name, name)
    if other_mentions(n, name, u):
        return TC.INCDEC_ALSO_USES % (name, name)  # FR-259's rule, and its wording
    return None


def _replace(n, old, new):
    if n is old:
        return new
    if isinstance(n, dict):
        return {k: (_replace(v, old, new) if k not in ("type", "referencedDecl", "range", "loc") else v)
                for k, v in n.items()}
    if isinstance(n, list):
        return [_replace(v, old, new) for v in n]
    return n


def _const(t):
    t = dict(t)
    for k in ("qualType", "desugaredQualType"):
        if k in t:
            t[k] = "const " + t[k]
    return t


def incdec_apply(n, u, fresh=None, vid=None):
    """``n`` with ``u`` (``x++``) read as ``x``, then ``++x;`` -- a
    ``return E`` through ``const T fresh = E;`` (decl id ``vid``)."""
    op = u["inner"][0]
    rng = u.get("range")
    read = {"kind": "ImplicitCastExpr", "range": rng, "type": op.get("type"), "valueCategory": "prvalue",
            "castKind": "LValueToRValue", "inner": [op]}
    step = {"kind": "UnaryOperator", "range": rng, "type": op.get("type"), "valueCategory": "lvalue",
            "isPostfix": False, "opcode": u["opcode"], "inner": [op]}
    new = _replace(n, u, read)
    if n.get("kind") != "ReturnStmt":
        return [new, step]
    e = new["inner"][0]
    nrng = n.get("range")
    T = _const(e["type"])
    vd = {"kind": "VarDecl", "id": vid, "range": nrng, "name": fresh, "type": T, "init": "c", "inner": [e]}
    ref = {"kind": "DeclRefExpr", "range": nrng, "type": T, "valueCategory": "lvalue",
           "referencedDecl": {"id": vid, "kind": "VarDecl", "name": fresh, "type": T}}
    ret = {"kind": "ReturnStmt", "range": nrng,
           "inner": [{"kind": "ImplicitCastExpr", "range": nrng, "type": e["type"], "valueCategory": "prvalue",
                      "castKind": "LValueToRValue", "inner": [ref]}]}
    return [{"kind": "DeclStmt", "range": nrng, "inner": [vd]}, step, ret]


FOR_TO_WHILE = Rule("for->while", "ForStmt", for_guard, for_apply)
DO_WHILE = Rule("do-while", "DoStmt", do_guard, do_apply)
# R3 matches a STATEMENT around one increment: guard(n, u, conditional),
# apply(n, u, fresh, vid) (the importer finds ``u`` and owns the fresh name)
POST_INCDEC = Rule("post-incdec", "Stmt", incdec_guard, incdec_apply)
RULES = (FOR_TO_WHILE, DO_WHILE, POST_INCDEC)
