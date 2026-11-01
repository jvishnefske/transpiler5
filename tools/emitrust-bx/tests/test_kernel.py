# Pins the Kernel's equality contract (FR-250): nodes compare STRUCTURALLY
# and source locations / surface-spelling hints never take part, because
# the bijection laws are stated as `parse(print(k)) == k` and a printed
# program can never reproduce the original columns.  Also pins the
# Surface-IR seam FR-254 needs: residuals are collected (all of them,
# sorted by location) and subset_distance counts them.
from emitrust_bx import kernel as K


def test_loc_excluded_from_equality():
    a = K.Binary("add", K.Var("x", K.Loc(1, 2)), K.IntLit(1, K.I32, K.Loc(1, 6)), K.Loc(1, 2))
    b = K.Binary("add", K.Var("x", K.Loc(9, 9)), K.IntLit(1, K.I32), None)
    assert a == b
    assert hash(a) == hash(b)


def test_spelling_excluded_from_equality():
    a = K.Binary("add", K.Var("x"), K.Var("y"), spelling="plain")
    b = K.Binary("add", K.Var("x"), K.Var("y"), spelling="wrapping")
    assert a == b


def test_structure_is_compared():
    assert K.IntLit(1, K.I32) != K.IntLit(1, K.I64)
    assert K.FloatLit("0.0") != K.FloatLit("1.0")
    assert K.Let("x", K.I32, True, K.IntLit(0, K.I32)) != K.Let("x", K.I32, False, K.IntLit(0, K.I32))


def test_residuals_collected_and_sorted():
    prog = K.Program((
        K.Func("main", (), K.UNIT, K.Block((
            K.ResidualStmt("b", K.Loc(5, 1)),
            K.Let("x", K.I32, True, K.ResidualExpr("a", K.Loc(3, 9))),
            K.If(K.BoolLit(True), K.Block((K.ResidualStmt("c", K.Loc(7, 2)),))),
        ))),
        K.ResidualItem("d", K.Loc(1, 1)),
    ))
    rs = K.residuals(prog)
    assert [r.reason for r in rs] == ["d", "a", "b", "c"]
    assert K.subset_distance(prog) == 4


def test_dump_is_location_free_and_stable():
    f = K.Func("f", (K.Param("a", K.I32, True, K.Loc(1, 1)),), K.I32,
               K.Block((K.Return(K.Binary("mul", K.Var("a"), K.IntLit(2, K.I32)), K.Loc(2, 3)),)))
    assert K.dump(K.Program((f,))) == (
        "func f(mut a: i32) -> i32\n"
        "  block\n"
        "    return (mul a 2:i32)\n")
