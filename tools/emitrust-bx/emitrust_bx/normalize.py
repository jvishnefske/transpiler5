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
"""

from dataclasses import dataclass
from typing import Callable, List, Optional

LOOPS = ("ForStmt", "WhileStmt", "DoStmt", "CXXForRangeStmt")
# a `continue` inside these cannot be bound to an enclosing loop
OPAQUE = ("LambdaExpr", "CXXRecordDecl", "FunctionDecl", "BlockExpr")

FOR_CONTINUE = ("`for` loop with a `continue` bound to it (normalize for->while: the `while` form's "
                "`continue` would skip the step)")
FOR_CVAR = "`for` loop with a condition variable (normalize for->while: no rule)"
DO_CONTINUE = ("do-while loop with a `continue` bound to it (normalize do-while: the `while (true)` form's "
               "`continue` would skip the condition)")


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


FOR_TO_WHILE = Rule("for->while", "ForStmt", for_guard, for_apply)
DO_WHILE = Rule("do-while", "DoStmt", do_guard, do_apply)
RULES = (FOR_TO_WHILE, DO_WHILE)
