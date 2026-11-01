# Pins Kernel well-formedness and the normal-form rules (FR-250).  The
# bijection laws are stated over NORMAL, well-typed Kernel terms, so every
# rule here is load-bearing: a missing check lets a printer emit code one
# compiler rejects (silently unbuildable), and a missing normalization
# makes parse(print(k)) != k for a term the generator can produce.
from emitrust_bx import kernel as K
from emitrust_bx import typecheck as TC

I = lambda v, t=K.I32: K.IntLit(v, t)  # noqa: E731


def prog(*stmts, ret=K.UNIT, params=()):
    return K.Program((K.Func("f", params, ret, K.Block(tuple(stmts))),))


def msgs(p, **kw):
    return [m for _, m in TC.check(p, **kw)]


def test_well_typed_program_has_no_errors():
    p = prog(K.Let("x", K.I64, True, I(1, K.I64)),
             K.CompoundAssign("add", "x", K.Cast(K.I64, K.Var("y"))), params=(K.Param("y", K.I32, False),))
    assert msgs(p) == []


def test_errors_are_collected_not_first_failure():
    p = prog(K.Let("a", K.I32, True, I(1, K.I64)),
             K.Let("b", K.I64, True, I(1)),
             K.Assign("c", I(0)))
    assert msgs(p) == ["expression has type i64, expected i32",
                       "expression has type i32, expected i64",
                       "assignment to undeclared 'c'"]


def test_kernel_has_no_implicit_conversions():
    assert msgs(prog(K.Let("x", K.I64, True, K.Binary("add", I(1, K.I64), I(1))))) == [
        "operands of add have different types i64 and i32"]


def test_shift_amount_may_differ():
    assert msgs(prog(K.Let("x", K.U64, True, K.Binary("shl", I(1, K.U64), I(3))))) == []


def test_no_cast_to_bool_and_bool_only_to_ints():
    assert msgs(prog(K.Let("b", K.BOOL, True, K.Cast(K.BOOL, I(1))))) == ["cast to bool"]
    assert msgs(prog(K.Let("d", K.F64, True, K.Cast(K.F64, K.BoolLit(True))))) == ["cast from bool to f64"]


def test_identifiers_must_be_legal_on_both_sides():
    for bad, why in [("match", "is a Rust keyword"), ("class", "is a C++ keyword"),
                     ("None", "is a Rust keyword"), ("_x", "starts with an underscore"),
                     ("a__b", "double underscore"), ("printf", "reserved")]:
        m = msgs(prog(K.Let(bad, K.I32, True, I(0))))
        assert len(m) == 1 and why in m[0], (bad, m)


def test_same_scope_redeclaration_rejected_nested_allowed():
    assert msgs(prog(K.Let("x", K.I32, True, I(0)), K.Let("x", K.I32, True, I(0)))) == [
        "redeclaration of 'x' in the same scope"]
    assert msgs(prog(K.Let("x", K.I32, True, I(0)), K.Block((K.Let("x", K.I64, True, I(0, K.I64)),)))) == []
    # a parameter shares the body's outermost scope (C++ rule)
    assert msgs(prog(K.Let("p", K.I32, True, I(0)), params=(K.Param("p", K.I32, True),))) == [
        "redeclaration of 'p' in the same scope"]


def test_counting_loop_rules():
    body_assigns_i = K.Block((K.Assign("i", I(3)),))
    m = msgs(prog(K.For("i", I(0), I(10), body_assigns_i)))
    assert "counting loop body assigns the loop variable 'i'" in m
    m = msgs(prog(K.Let("n", K.I32, True, I(5)),
                  K.For("i", I(0), K.Var("n"), K.Block((K.CompoundAssign("add", "n", I(1)),)))))
    assert m == ["counting loop body assigns or shadows the bound 'n'"]
    m = msgs(prog(K.For("i", I(0), K.Binary("add", I(1), I(2)), K.Block(()))))
    assert m == ["counting loop bound must be a literal or a variable"]
    m = msgs(prog(K.For("i", I(0, K.I64), I(9, K.I64), K.Block(()))))
    assert m == ["counting loop variable of type i64 (only int and std::size_t)"]


def test_main_and_returns():
    p = K.Program((K.Func("main", (), K.UNIT, K.Block((K.Return(None),))),))
    assert msgs(p) == ["main may not contain return"]
    p = prog(K.If(K.BoolLit(True), K.Block((K.Return(I(1)),))), ret=K.I32)
    assert msgs(p) == ["function 'f' may reach its end without returning a value"]
    p = prog(K.If(K.BoolLit(True), K.Block((K.Return(I(1)),)), K.Block((K.Return(I(2)),))), ret=K.I32)
    assert msgs(p) == []


def test_break_outside_loop():
    assert msgs(prog(K.Break())) == ["break outside a loop"]


def test_printf_spec_typing():
    p = prog(K.Print((K.Spec("d", K.FloatLit("1.0")),)))
    assert msgs(p) == ["printf integer conversion of a f64 argument"]
    p = prog(K.Print((K.Spec("c", I(65, K.I8)),)))
    assert msgs(p) == ["printf %c of a i8 argument"]
    p = prog(K.Print((K.Spec("s", K.Var("x")),)))
    assert msgs(p) == ["printf %s argument must be a string literal"]


def test_void_call_is_not_a_value():
    p = K.Program((K.Func("g", (), K.UNIT, K.Block(())),
                   K.Func("f", (), K.I32, K.Block((K.Return(K.Call("g", ())),)))))
    assert msgs(p) == ["call to void function 'g' used as a value"]


def test_rust_surface_spelling_rules():
    plain_u = prog(K.Let("x", K.U32, True, K.Binary("add", I(1, K.U32), I(2, K.U32), spelling="plain")))
    assert msgs(plain_u, surface="rust") == [
        "unsupported: unsigned `+` (panics on overflow in debug builds); "
        "the subset spells it `.wrapping_add(..)`"]
    wrap_i = prog(K.Let("x", K.I32, True, K.Binary("mul", I(1), I(2), spelling="wrapping")))
    assert msgs(wrap_i, surface="rust") == [
        "unsupported: `.wrapping_mul` on i32 (signed overflow is undefined behaviour in C++)"]
    cmp_u = prog(K.Let("x", K.U32, True, I(0, K.U32)), K.CompoundAssign("add", "x", I(1, K.U32), spelling="plain"))
    assert msgs(cmp_u, surface="rust") == [
        "unsupported: unsigned `+=` (panics on overflow in debug builds); "
        "the subset spells it `x = x.wrapping_add(..)`"]


def test_normal_forms():
    n = TC.normalize
    # N1 identity cast, N2 literal fold, N3 int->f64 literal, N4 negative literal
    p = prog(K.Let("a", K.I32, True, K.Cast(K.I32, I(1))),
             K.Let("b", K.I64, True, K.Cast(K.I64, I(7))),
             K.Let("c", K.F64, True, K.Cast(K.F64, I(3))),
             K.Let("d", K.I64, True, K.Cast(K.I64, K.Unary("neg", I(5)))),
             K.Let("e", K.U8, True, K.Cast(K.U8, I(300))),
             K.Assign("a", K.Binary("add", K.Var("a"), I(2))),
             K.Assign("a", K.Binary("sub", I(2), K.Var("a"))),
             K.Print((K.Text("a"), K.Text("b"), K.Text(""))))
    got = n(p).funcs()[0].body.stmts
    assert got[0].init == I(1)
    assert got[1].init == I(7, K.I64)
    assert got[2].init == K.FloatLit("3.0")
    assert got[3].init == K.Unary("neg", I(5, K.I64))
    assert got[4].init == K.Cast(K.U8, I(300))  # does not fit: stays a cast
    assert got[5] == K.CompoundAssign("add", "a", I(2))  # N5
    assert isinstance(got[6], K.Assign)  # x = 2 - x is not x -= 2
    assert got[7] == K.Print((K.Text("ab"),))  # N6
    assert n(n(p)) == n(p)


def test_point_of_declaration_differs_so_self_reference_is_rejected():
    # Rust `let x = x + 1;` reads the OUTER x; C++ `int x = x + 1;` reads the
    # new, uninitialized one. Same for a counting loop's bounds.
    p = prog(K.Let("x", K.I32, True, I(1)),
             K.Block((K.Let("x", K.I32, True, K.Binary("add", K.Var("x"), I(1))),)),
             K.For("x", I(0), K.Var("x"), K.Block(())))
    assert msgs(p) == ["initializer of 'x' refers to a variable of the same name",
                       "counting loop bounds refer to a variable of the loop variable's name 'x'"]
