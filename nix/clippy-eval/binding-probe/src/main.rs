//! binding-probe -- the AST half of the FR "binding hygiene" metric.
//!
//! It answers, over emitted Rust and with a REAL parser (`syn`), two
//! deliberately separate questions:
//!
//!   Axis A  how many `let` bindings are FREE single-use temporaries -- the
//!           `let v5 = f(x); g(v5);` chains that are the most obvious tell
//!           that a machine wrote this code.
//!   Axis B  how deeply nested are the statements' expressions -- the
//!           anti-cramming guard, so nobody "improves" axis A by welding
//!           statements together.
//!
//! The two are NEVER summed. Either one alone is gameable in one direction;
//! that is the entire reason the metric is two-dimensional.
//!
//! A binding counts toward axis A only when ALL FOUR hold:
//!   1. the name is the emitter's generated form (`v<N>`), not a meaningful
//!      name lifted from the C source;
//!   2. it has exactly one use in its scope;
//!   3. that use is in the IMMEDIATELY following statement of the same block;
//!   4. inlining it would cross no side effect.
//!
//! Condition 4 is the load-bearing one, and it is what makes the axis both
//! non-gameable and CORRECT. A temporary that cannot be inlined without
//! moving an evaluation across a side effect is not noise -- it is
//! load-bearing evaluation order, and "fixing" it would be a miscompile.
//! This is the same question `laterArgSideEffects`
//! (lib/ImportC/ImportCStatements.cpp) asks importer-side for FR-192, run in
//! the opposite direction: FR-192 asks "do LATER arguments have effects, so
//! must this read be DEFERRED?"; here we ask "does anything evaluated BEFORE
//! the use site have effects, so must this binding STAY PUT?".
//!
//! Everything here is conservative in the direction of NOT counting a
//! binding: an unknown expression form, an unparseable macro body, a
//! shadowed name, or a use in a conditionally-evaluated position all reject
//! the candidate and are reported in their own bucket. Axis A is therefore a
//! LOWER bound on the debt, and the buckets show exactly how much is being
//! left on the table.

use std::collections::HashMap;
use std::io::Read;

use syn::punctuated::Punctuated;
use syn::visit::{self, Visit};
use syn::{Block, Expr, Ident, Item, Lit, Local, Pat, Stmt, Token};

// ---------------------------------------------------------------------------
// purity
// ---------------------------------------------------------------------------

/// How much an expression is allowed to disturb, ordered.
///
/// `Pure` cannot write memory, perform I/O, or diverge. `MayPanic` cannot
/// write or perform I/O but can abort the statement (an index out of bounds,
/// an arithmetic overflow -- the emitted crates carry no `[profile]`
/// override, so they build in debug and overflow checks are ON).
/// `Effectful` can do anything.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Purity {
    Pure,
    MayPanic,
    Effectful,
}

fn purity(e: &Expr) -> Purity {
    use Purity::*;
    let kids = |v: Vec<&Expr>| v.into_iter().map(purity).max().unwrap_or(Pure);
    match e {
        Expr::Lit(_) | Expr::Path(_) | Expr::Infer(_) => Pure,
        Expr::Paren(p) => purity(&p.expr),
        Expr::Group(g) => purity(&g.expr),
        Expr::Field(f) => purity(&f.base),
        Expr::Cast(c) => purity(&c.expr),
        Expr::Tuple(t) => kids(t.elems.iter().collect()),
        Expr::Array(a) => kids(a.elems.iter().collect()),
        Expr::Repeat(r) => purity(&r.expr),
        Expr::Struct(s) => kids(s.fields.iter().map(|f| &f.expr).collect()).max(
            s.rest.as_deref().map(purity).unwrap_or(Pure),
        ),
        Expr::Range(r) => kids(
            [r.start.as_deref(), r.end.as_deref()]
                .into_iter()
                .flatten()
                .collect(),
        ),
        // Forming a shared reference is pure; forming a UNIQUE one is not --
        // it can invalidate a read the moved expression was going to make.
        Expr::Reference(r) => {
            if r.mutability.is_some() {
                Effectful
            } else {
                purity(&r.expr)
            }
        }
        // Indexing can panic.
        Expr::Index(i) => MayPanic.max(purity(&i.expr)).max(purity(&i.index)),
        Expr::Unary(u) => match u.op {
            syn::UnOp::Not(_) => purity(&u.expr),
            // `-x` overflows on MIN; `-1i32` (a literal) cannot.
            syn::UnOp::Neg(_) => {
                if matches!(&*u.expr, Expr::Lit(_)) {
                    Pure
                } else {
                    MayPanic.max(purity(&u.expr))
                }
            }
            // A deref may observe a write the moved expression performs, and
            // a raw-pointer deref is unbounded.
            _ => Effectful,
        },
        Expr::Binary(b) => {
            use syn::BinOp::*;
            let own = match b.op {
                Add(_) | Sub(_) | Mul(_) | Div(_) | Rem(_) | Shl(_) | Shr(_) => MayPanic,
                And(_) | Or(_) | BitAnd(_) | BitOr(_) | BitXor(_) | Eq(_) | Lt(_) | Le(_)
                | Ne(_) | Ge(_) | Gt(_) => Pure,
                _ => Effectful, // the compound assignments
            };
            own.max(purity(&b.left)).max(purity(&b.right))
        }
        _ => Effectful,
    }
}

// ---------------------------------------------------------------------------
// census: scope-aware use counting
// ---------------------------------------------------------------------------

/// Counts variable READS (single-segment path expressions) and BINDINGS
/// (`PatIdent`, which also covers closure parameters, `for` patterns and
/// match-arm bindings -- every way a name can be shadowed) within one scope,
/// and collects string literals so an inline format argument (`{v5}`), which
/// is a use the AST cannot see, can be detected and excluded.
///
/// `visit_item` is overridden to a no-op so a nested item (a nested `fn`,
/// most importantly) is NOT folded into the enclosing scope's census; it
/// gets its own scope.
#[derive(Default)]
struct Census {
    reads: HashMap<String, usize>,
    binds: HashMap<String, usize>,
    strings: Vec<String>,
}

impl<'ast> Visit<'ast> for Census {
    fn visit_item(&mut self, _: &'ast Item) {}

    fn visit_expr_path(&mut self, e: &'ast syn::ExprPath) {
        if e.qself.is_none() && e.path.leading_colon.is_none() && e.path.segments.len() == 1 {
            let seg = &e.path.segments[0];
            if seg.arguments.is_none() {
                *self.reads.entry(seg.ident.to_string()).or_default() += 1;
            }
        }
        visit::visit_expr_path(self, e);
    }

    fn visit_pat_ident(&mut self, p: &'ast syn::PatIdent) {
        *self.binds.entry(p.ident.to_string()).or_default() += 1;
        visit::visit_pat_ident(self, p);
    }

    fn visit_lit(&mut self, l: &'ast Lit) {
        if let Lit::Str(s) = l {
            self.strings.push(s.value());
        }
        visit::visit_lit(self, l);
    }

    fn visit_macro(&mut self, m: &'ast syn::Macro) {
        // A macro body is raw tokens; `visit` will not descend. Parse the
        // usual comma-separated-expression shape (which is what every macro
        // the emitter produces is) so its uses are counted; if that fails,
        // fall back to scanning the token stream for the bare identifier,
        // which over-counts rather than missing a use.
        if let Ok(args) = m.parse_body_with(Punctuated::<Expr, Token![,]>::parse_terminated) {
            for a in &args {
                self.visit_expr(a);
            }
        } else {
            let mut idents = Vec::new();
            collect_token_idents(m.tokens.clone(), &mut idents);
            for i in idents {
                *self.reads.entry(i).or_default() += 1;
            }
        }
        visit::visit_macro(self, m);
    }
}

fn collect_token_idents(ts: proc_macro2::TokenStream, out: &mut Vec<String>) {
    for t in ts {
        match t {
            proc_macro2::TokenTree::Ident(i) => out.push(i.to_string()),
            proc_macro2::TokenTree::Group(g) => collect_token_idents(g.stream(), out),
            _ => {}
        }
    }
}

fn census_of_block(b: &Block) -> Census {
    let mut c = Census::default();
    c.visit_block(b);
    c
}

fn census_of_stmt(s: &Stmt) -> Census {
    let mut c = Census::default();
    c.visit_stmt(s);
    c
}

/// `{name}` / `{name:spec}` inside a format string is a USE that no AST walk
/// can see. Detect it textually so such a binding is skipped rather than
/// silently counted as single-use.
fn string_mentions(strings: &[String], name: &str) -> bool {
    let open = format!("{{{name}");
    strings.iter().any(|s| {
        let mut from = 0usize;
        while let Some(i) = s[from..].find(&open) {
            let at = from + i + open.len();
            match s[at..].chars().next() {
                Some('}') | Some(':') => return true,
                _ => {}
            }
            from = at;
        }
        false
    })
}

// ---------------------------------------------------------------------------
// locating the use and the evaluation prefix
// ---------------------------------------------------------------------------

/// Where the single use sits, and how disturbing the evaluation prefix is.
///
/// `Found` carries the PURITY of everything evaluated strictly before the use
/// rather than the subexpressions themselves, which keeps the whole traversal
/// free of borrows -- the macro path has to parse an owned argument list, so
/// it could not hand back references into it anyway.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Loc {
    /// The name does not occur in this subtree.
    Absent,
    /// It occurs, but in a position whose evaluation order relative to the
    /// binding cannot be established (a conditional branch, a loop body, a
    /// closure, a compound assignment, an unmodelled expression form).
    Unordered,
    /// It occurs as the base of a borrow or on the left of an assignment --
    /// a PLACE, not a value. Inlining changes what is being borrowed (and can
    /// turn a named local into a temporary whose lifetime ends too early).
    Place,
    /// Found, carrying the worst purity of what is evaluated before it and
    /// whether the use is a PLACE use -- the base of a field access, an
    /// index, or a method receiver.
    ///
    /// A place use does not MOVE the value, so inlining changes WHEN it is
    /// dropped: from the end of the enclosing block to the end of the
    /// consuming statement. For a type with a side-effecting `Drop` that is
    /// observable, and the probe has no type information to rule it out --
    /// only the `let`'s syntactic annotation (see `type_is_certainly_nondrop`).
    Found(Purity, bool),
}

fn contains_ident(e: &Expr, name: &str) -> bool {
    let mut c = Census::default();
    c.visit_expr(e);
    c.reads.get(name).copied().unwrap_or(0) > 0 || string_mentions(&c.strings, name)
}

/// Walk children in EVALUATION order, accumulating the prefix's purity.
fn seq(children: Vec<&Expr>, name: &str) -> Loc {
    let mut prefix = Purity::Pure;
    for c in children {
        match locate(c, name) {
            Loc::Absent => prefix = prefix.max(purity(c)),
            Loc::Unordered => return Loc::Unordered,
            Loc::Place => return Loc::Place,
            Loc::Found(inner, place) => {
                return Loc::Found(prefix.max(inner), place)
            }
        }
    }
    Loc::Absent
}

/// Mark a `Found` as a place (non-moving) use.
fn place_use(l: Loc) -> Loc {
    match l {
        Loc::Found(p, _) => Loc::Found(p, true),
        other => other,
    }
}

/// Can this type be ruled out, from SYNTAX ALONE, as having a `Drop` impl?
///
/// Only primitives, references, raw pointers, and arrays/tuples/slices of
/// those. Anything named (`String`, `Vec<_>`, a lifted C++ class with a
/// destructor, a generated enum) is treated as possibly-Drop, because the
/// probe parses one file with no type resolution and CANNOT know.
fn type_is_certainly_nondrop(ty: &syn::Type) -> bool {
    const PRIMS: &[&str] = &[
        "bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128",
        "isize", "u8", "u16", "u32", "u64", "u128", "usize",
    ];
    match ty {
        syn::Type::Reference(_) | syn::Type::Ptr(_) | syn::Type::BareFn(_) => true,
        syn::Type::Paren(p) => type_is_certainly_nondrop(&p.elem),
        syn::Type::Group(g) => type_is_certainly_nondrop(&g.elem),
        syn::Type::Array(a) => type_is_certainly_nondrop(&a.elem),
        syn::Type::Slice(s) => type_is_certainly_nondrop(&s.elem),
        syn::Type::Tuple(t) => t.elems.iter().all(type_is_certainly_nondrop),
        syn::Type::Path(p) => {
            p.qself.is_none()
                && p.path.segments.len() == 1
                && p.path.segments[0].arguments.is_none()
                && PRIMS.contains(&p.path.segments[0].ident.to_string().as_str())
        }
        _ => false,
    }
}

fn is_target_path(e: &Expr, name: &str) -> bool {
    match e {
        Expr::Path(p) => {
            p.qself.is_none()
                && p.path.leading_colon.is_none()
                && p.path.segments.len() == 1
                && p.path.segments[0].arguments.is_none()
                && p.path.segments[0].ident == name
        }
        Expr::Paren(p) => is_target_path(&p.expr, name),
        Expr::Group(g) => is_target_path(&g.expr, name),
        _ => false,
    }
}

fn locate(e: &Expr, name: &str) -> Loc {
    if is_target_path(e, name) {
        return Loc::Found(Purity::Pure, false);
    }
    let guard = |cond: bool| if cond { Loc::Unordered } else { Loc::Absent };
    match e {
        Expr::Lit(_) | Expr::Path(_) | Expr::Infer(_) => Loc::Absent,
        Expr::Paren(p) => locate(&p.expr, name),
        Expr::Group(g) => locate(&g.expr, name),
        Expr::Cast(c) => locate(&c.expr, name),
        Expr::Field(f) => place_use(locate(&f.base, name)),
        Expr::Try(t) => locate(&t.expr, name),
        Expr::Await(a) => locate(&a.base, name),
        Expr::Let(l) => locate(&l.expr, name),

        // A borrow (of either mutability) makes the operand a PLACE.
        Expr::Reference(r) => {
            if contains_ident(&r.expr, name) {
                Loc::Place
            } else {
                Loc::Absent
            }
        }
        // `*p` as an rvalue is fine to have the target under; as an lvalue
        // the enclosing Assign arm catches it first.
        Expr::Unary(u) => locate(&u.expr, name),

        Expr::Call(c) => {
            let mut kids: Vec<&Expr> = vec![&c.func];
            kids.extend(c.args.iter());
            seq(kids, name)
        }
        Expr::MethodCall(m) => {
            if contains_ident(&m.receiver, name) {
                // The receiver is evaluated first, so the prefix is empty;
                // an autoref'd receiver does not move the value.
                place_use(locate(&m.receiver, name))
            } else {
                let mut kids: Vec<&Expr> = vec![&m.receiver];
                kids.extend(m.args.iter());
                seq(kids, name)
            }
        }
        Expr::Index(i) => {
            if contains_ident(&i.expr, name) {
                place_use(locate(&i.expr, name))
            } else {
                seq(vec![&i.expr, &i.index], name)
            }
        }
        Expr::Tuple(t) => seq(t.elems.iter().collect(), name),
        Expr::Array(a) => seq(a.elems.iter().collect(), name),
        Expr::Repeat(r) => locate(&r.expr, name),
        Expr::Struct(s) => {
            let mut kids: Vec<&Expr> = s.fields.iter().map(|f| &f.expr).collect();
            if let Some(rest) = s.rest.as_deref() {
                kids.push(rest);
            }
            seq(kids, name)
        }
        Expr::Range(r) => seq(
            [r.start.as_deref(), r.end.as_deref()]
                .into_iter()
                .flatten()
                .collect(),
            name,
        ),
        Expr::Binary(b) => {
            use syn::BinOp::*;
            match b.op {
                // Short-circuit: a use on the right is CONDITIONALLY
                // evaluated, so sinking the binding into it changes when (or
                // whether) the initializer runs.
                And(_) | Or(_) => {
                    if contains_ident(&b.right, name) {
                        Loc::Unordered
                    } else {
                        locate(&b.left, name)
                    }
                }
                Add(_) | Sub(_) | Mul(_) | Div(_) | Rem(_) | Shl(_) | Shr(_) | BitAnd(_)
                | BitOr(_) | BitXor(_) | Eq(_) | Lt(_) | Le(_) | Ne(_) | Ge(_) | Gt(_) => {
                    seq(vec![&b.left, &b.right], name)
                }
                // Compound assignment: the place is evaluated after the
                // value, and the place is written. Not modelled.
                _ => guard(contains_ident(e, name)),
            }
        }
        Expr::Assign(a) => {
            if contains_ident(&a.left, name) {
                Loc::Place
            } else {
                // Rust evaluates the right operand first, so a use there has
                // an empty prefix.
                locate(&a.right, name)
            }
        }
        Expr::Return(r) => match r.expr.as_deref() {
            Some(x) => locate(x, name),
            None => Loc::Absent,
        },
        Expr::Break(b) => match b.expr.as_deref() {
            Some(x) => locate(x, name),
            None => Loc::Absent,
        },
        // The scrutinee / condition / iterator runs unconditionally exactly
        // once; anything in a branch or body does not.
        Expr::If(i) => {
            if contains_ident(&i.cond, name) {
                locate(&i.cond, name)
            } else {
                guard(
                    block_contains(&i.then_branch, name)
                        || i.else_branch
                            .as_ref()
                            .is_some_and(|(_, e)| contains_ident(e, name)),
                )
            }
        }
        Expr::Match(m) => {
            if contains_ident(&m.expr, name) {
                locate(&m.expr, name)
            } else {
                guard(m.arms.iter().any(|a| {
                    contains_ident(&a.body, name)
                        || a.guard.as_ref().is_some_and(|(_, g)| contains_ident(g, name))
                }))
            }
        }
        Expr::ForLoop(f) => {
            if contains_ident(&f.expr, name) {
                locate(&f.expr, name)
            } else {
                guard(block_contains(&f.body, name))
            }
        }
        Expr::Macro(m) => locate_macro(&m.mac, name),
        _ => guard(contains_ident(e, name)),
    }
}

fn block_contains(b: &Block, name: &str) -> bool {
    let c = census_of_block(b);
    c.reads.get(name).copied().unwrap_or(0) > 0 || string_mentions(&c.strings, name)
}

fn locate_macro(m: &syn::Macro, name: &str) -> Loc {
    // Every macro the emitter produces is a comma-separated expression list
    // (`println!`, `write!`, `assert!`, `format!`, ...), whose arguments are
    // evaluated left to right.
    match m.parse_body_with(Punctuated::<Expr, Token![,]>::parse_terminated) {
        Ok(args) => {
            // A `{name}` inline format argument is a use in the format
            // string, which parses as a plain literal. Refuse to reason.
            for a in &args {
                if let Expr::Lit(l) = a {
                    if let Lit::Str(s) = &l.lit {
                        if string_mentions(&[s.value()], name) {
                            return Loc::Unordered;
                        }
                    }
                }
            }
            let owned: Vec<Expr> = args.into_iter().collect();
            seq(owned.iter().collect(), name)
        }
        Err(_) => {
            let mut idents = Vec::new();
            collect_token_idents(m.tokens.clone(), &mut idents);
            if idents.iter().any(|i| i == name) {
                Loc::Unordered
            } else {
                Loc::Absent
            }
        }
    }
}

/// Locate the use inside a whole STATEMENT.
fn locate_stmt(s: &Stmt, name: &str) -> Loc {
    match s {
        Stmt::Local(l) => match &l.init {
            Some(init) if init.diverge.is_none() => locate(&init.expr, name),
            Some(_) => Loc::Unordered,
            None => Loc::Absent,
        },
        Stmt::Expr(e, _) => locate(e, name),
        Stmt::Macro(m) => locate_macro(&m.mac, name),
        Stmt::Item(_) => Loc::Absent,
    }
}

// ---------------------------------------------------------------------------
// axis B: expression nesting depth
// ---------------------------------------------------------------------------

/// Depth of a statement's expression tree, NOT descending through a block
/// boundary: the statements of a nested block are counted as their own
/// statements, so this measures EXPRESSION nesting and not brace nesting.
///
/// A literal or a path is 1; `f(x)` is 2; `f(g(x))` is 3.
///
/// A chain of the same-precedence binary operator is ONE level, not one per
/// operator: `a + b + c + d` parses left-associatively, so the naive AST
/// depth would call a flat sum depth 4 and punish the emitter for arithmetic
/// it did not nest. An EXPLICIT paren resets the chain (`a + (b + c)` really
/// is nested), which is why `Expr::Paren` clears the context instead of
/// being transparent to it.
fn prec_class(op: &syn::BinOp) -> u8 {
    use syn::BinOp::*;
    match op {
        Mul(_) | Div(_) | Rem(_) => 0,
        Add(_) | Sub(_) => 1,
        Shl(_) | Shr(_) => 2,
        BitAnd(_) => 3,
        BitXor(_) => 4,
        BitOr(_) => 5,
        Eq(_) | Lt(_) | Le(_) | Ne(_) | Ge(_) | Gt(_) => 6,
        And(_) => 7,
        Or(_) => 8,
        _ => 9,
    }
}

fn depth(e: &Expr) -> usize {
    depth_ctx(e, None)
}

fn depth_ctx(e: &Expr, chain: Option<u8>) -> usize {
    let kids = |v: Vec<&Expr>| 1 + v.into_iter().map(depth).max().unwrap_or(0);
    match e {
        Expr::Lit(_) | Expr::Path(_) | Expr::Infer(_) | Expr::Continue(_) => 1,
        // An explicit paren is not a level of its own, but it DOES break an
        // operator chain.
        Expr::Paren(p) => depth_ctx(&p.expr, None),
        Expr::Group(g) => depth_ctx(&g.expr, chain),
        Expr::Binary(b) => {
            let cls = prec_class(&b.op);
            let add = usize::from(chain != Some(cls));
            add + depth_ctx(&b.left, Some(cls)).max(depth_ctx(&b.right, Some(cls)))
        }
        Expr::Cast(c) => kids(vec![&c.expr]),
        Expr::Field(f) => kids(vec![&f.base]),
        Expr::Try(t) => kids(vec![&t.expr]),
        Expr::Await(a) => kids(vec![&a.base]),
        Expr::Let(l) => kids(vec![&l.expr]),
        Expr::Reference(r) => kids(vec![&r.expr]),
        Expr::Unary(u) => kids(vec![&u.expr]),
        Expr::Assign(a) => kids(vec![&a.left, &a.right]),
        Expr::Index(i) => kids(vec![&i.expr, &i.index]),
        Expr::Repeat(r) => kids(vec![&r.expr]),
        Expr::Call(c) => kids(c.args.iter().collect()),
        Expr::MethodCall(m) => {
            let mut k: Vec<&Expr> = vec![&m.receiver];
            k.extend(m.args.iter());
            kids(k)
        }
        Expr::Tuple(t) => kids(t.elems.iter().collect()),
        Expr::Array(a) => kids(a.elems.iter().collect()),
        Expr::Struct(s) => kids(s.fields.iter().map(|f| &f.expr).collect()),
        Expr::Range(r) => kids(
            [r.start.as_deref(), r.end.as_deref()]
                .into_iter()
                .flatten()
                .collect(),
        ),
        Expr::Return(r) => kids(r.expr.as_deref().into_iter().collect()),
        Expr::Break(b) => kids(b.expr.as_deref().into_iter().collect()),
        // Block-bearing forms: only the operand evaluated in THIS statement,
        // never the block (whose statements are counted apart).
        Expr::If(i) => kids(vec![&i.cond]),
        Expr::Match(m) => kids(vec![&m.expr]),
        Expr::ForLoop(f) => kids(vec![&f.expr]),
        Expr::While(w) => kids(vec![&w.cond]),
        Expr::Loop(_) | Expr::Block(_) | Expr::Unsafe(_) | Expr::Async(_) | Expr::Const(_)
        | Expr::Closure(_) | Expr::TryBlock(_) => 1,
        Expr::Macro(m) => match m
            .mac
            .parse_body_with(Punctuated::<Expr, Token![,]>::parse_terminated)
        {
            Ok(args) => 1 + args.iter().map(depth).max().unwrap_or(0),
            Err(_) => 1,
        },
        _ => 1,
    }
}

fn stmt_depth(s: &Stmt) -> Option<usize> {
    match s {
        Stmt::Local(l) => Some(l.init.as_ref().map(|i| depth(&i.expr)).unwrap_or(1)),
        Stmt::Expr(e, _) => Some(depth(e)),
        Stmt::Macro(m) => Some(
            match m
                .mac
                .parse_body_with(Punctuated::<Expr, Token![,]>::parse_terminated)
            {
                Ok(args) => 1 + args.iter().map(depth).max().unwrap_or(0),
                Err(_) => 1,
            },
        ),
        Stmt::Item(_) => None,
    }
}

// ---------------------------------------------------------------------------
// the analysis
// ---------------------------------------------------------------------------

#[derive(Default, Debug)]
struct Stats {
    files: usize,
    parse_errors: usize,
    statements: usize,
    /// Every `let v<N> = ...` (condition 1), read form only.
    generated_lets: usize,
    /// `_v<N>` -- the emitter's UNREAD form. Zero uses by construction, so it
    /// can never satisfy condition 2; tracked only so the population is
    /// fully accounted for.
    unread_generated_lets: usize,
    /// Conditions 1-3 (what a scope-aware parser can see WITHOUT condition 4).
    single_use_next_stmt: usize,
    /// Conditions 1-4. THE AXIS A NUMERATOR.
    free_temps: usize,
    // Why a conditions-1-3 candidate did not become a free temp. These
    // buckets PARTITION `single_use_next_stmt - free_temps` exactly; nothing
    // is counted twice.
    blocked_effectful_prefix: usize,
    blocked_panicking_prefix: usize,
    blocked_place: usize,
    blocked_unordered: usize,
    /// A place (non-moving) use of a value whose type cannot be shown, from
    /// the annotation alone, to have no `Drop`.
    blocked_maybe_drop: usize,
    // rejected before condition 4 even applies
    skipped_shadowed: usize,
    skipped_inline_fmt: usize,
    skipped_mut: usize,
    depth_hist: HashMap<usize, usize>,
    sites: Vec<Site>,
    /// The deepest statements, so axis B is a WORK QUEUE too and not just a
    /// scalar -- "depth rose" is useless without "here is the statement".
    deep: Vec<Deep>,
}

#[derive(Debug)]
struct Deep {
    file: String,
    line: usize,
    depth: usize,
    text: String,
}

#[derive(Debug)]
struct Site {
    file: String,
    line: usize,
    name: String,
    verdict: String,
    text: String,
}

fn is_generated(name: &str) -> Option<bool> {
    // Returns Some(read_form) for the emitter's generated spellings
    // (TranslateToRust.cpp: `("v" or "_v") + counter`), None otherwise.
    let (read, digits) = if let Some(d) = name.strip_prefix("_v") {
        (false, d)
    } else if let Some(d) = name.strip_prefix('v') {
        (true, d)
    } else {
        return None;
    };
    if !digits.is_empty() && digits.bytes().all(|b| b.is_ascii_digit()) {
        Some(read)
    } else {
        None
    }
}

/// The `let`'s syntactic type annotation, if it has one.
fn local_type(l: &Local) -> Option<&syn::Type> {
    match &l.pat {
        Pat::Type(t) => Some(&t.ty),
        _ => None,
    }
}

fn local_binding(l: &Local) -> Option<(&Ident, bool)> {
    let pat = match &l.pat {
        Pat::Type(t) => &*t.pat,
        p => p,
    };
    match pat {
        Pat::Ident(pi) if pi.by_ref.is_none() && pi.subpat.is_none() => {
            Some((&pi.ident, pi.mutability.is_some()))
        }
        _ => None,
    }
}

struct ScopeCollector<'ast> {
    scopes: Vec<&'ast Block>,
}

impl<'ast> Visit<'ast> for ScopeCollector<'ast> {
    fn visit_item_fn(&mut self, f: &'ast syn::ItemFn) {
        self.scopes.push(&f.block);
        visit::visit_item_fn(self, f);
    }
    fn visit_impl_item_fn(&mut self, f: &'ast syn::ImplItemFn) {
        self.scopes.push(&f.block);
        visit::visit_impl_item_fn(self, f);
    }
    fn visit_trait_item_fn(&mut self, f: &'ast syn::TraitItemFn) {
        if let Some(b) = &f.default {
            self.scopes.push(b);
        }
        visit::visit_trait_item_fn(self, f);
    }
}

struct BlockCollector<'ast> {
    blocks: Vec<&'ast Block>,
}

impl<'ast> Visit<'ast> for BlockCollector<'ast> {
    fn visit_item(&mut self, _: &'ast Item) {}
    fn visit_block(&mut self, b: &'ast Block) {
        self.blocks.push(b);
        visit::visit_block(self, b);
    }
}

/// Statements at or beyond this depth are listed individually. 6 is one
/// past the p95 measured at the epoch-6 baseline, so the list is the tail
/// and not the bulk.
const DEEP_THRESHOLD: usize = 6;

fn stmt_line(s: &Stmt) -> usize {
    use syn::spanned::Spanned;
    s.span().start().line
}

fn render(s: &Stmt) -> String {
    let t = quote::ToTokens::to_token_stream(s).to_string();
    if t.chars().count() > 110 {
        let cut: String = t.chars().take(107).collect();
        format!("{cut}...")
    } else {
        t
    }
}

fn analyze_file(path: &str, src: &str, st: &mut Stats) {
    st.files += 1;
    let file = match syn::parse_file(src) {
        Ok(f) => f,
        Err(_) => {
            st.parse_errors += 1;
            return;
        }
    };
    let mut sc = ScopeCollector { scopes: Vec::new() };
    sc.visit_file(&file);

    for root in sc.scopes {
        let census = census_of_block(root);
        let mut bc = BlockCollector { blocks: Vec::new() };
        bc.visit_block(root);
        for block in bc.blocks {
            for (i, stmt) in block.stmts.iter().enumerate() {
                if let Some(d) = stmt_depth(stmt) {
                    st.statements += 1;
                    *st.depth_hist.entry(d).or_default() += 1;
                    if d >= DEEP_THRESHOLD {
                        st.deep.push(Deep {
                            file: path.to_string(),
                            line: stmt_line(stmt),
                            depth: d,
                            text: render(stmt),
                        });
                    }
                }
                let Stmt::Local(l) = stmt else { continue };
                let Some((ident, is_mut)) = local_binding(l) else {
                    continue;
                };
                let name = ident.to_string();
                let Some(read_form) = is_generated(&name) else {
                    continue;
                };
                if !read_form {
                    st.unread_generated_lets += 1;
                    continue;
                }
                st.generated_lets += 1;
                if l.init.as_ref().map(|i| i.diverge.is_some()).unwrap_or(true) {
                    continue;
                }
                let line = ident.span().start().line;
                let push = |st: &mut Stats, verdict: &str| {
                    st.sites.push(Site {
                        file: path.to_string(),
                        line,
                        name: name.clone(),
                        verdict: verdict.to_string(),
                        text: render(stmt),
                    });
                };

                // condition 1': the name must not be shadowed anywhere in the
                // scope, or "exactly one use" is not a question this census
                // can answer.
                if census.binds.get(&name).copied().unwrap_or(0) != 1 {
                    st.skipped_shadowed += 1;
                    push(&mut *st, "shadowed");
                    continue;
                }
                if string_mentions(&census.strings, &name) {
                    st.skipped_inline_fmt += 1;
                    push(&mut *st, "inline-fmt-arg");
                    continue;
                }
                // condition 2: exactly one use in scope.
                if census.reads.get(&name).copied().unwrap_or(0) != 1 {
                    continue;
                }
                // condition 3: that use is in the immediately following
                // statement of THIS block.
                let Some(next) = block.stmts.get(i + 1) else {
                    continue;
                };
                let nc = census_of_stmt(next);
                if nc.reads.get(&name).copied().unwrap_or(0) != 1 {
                    continue;
                }
                st.single_use_next_stmt += 1;

                // A `mut` binding is a PLACE by intent; inlining it turns a
                // named local into a temporary and its `&mut` borrows into
                // borrows of a temporary.
                if is_mut {
                    // Its OWN bucket, not folded into `blocked_place`: the
                    // buckets are reported as a partition of the
                    // conditions-1-3 candidates and must sum exactly.
                    st.skipped_mut += 1;
                    push(&mut *st, "mut-place");
                    continue;
                }

                // condition 4: inlining crosses no side effect.
                let init_purity = purity(&l.init.as_ref().unwrap().expr);
                match locate_stmt(next, &name) {
                    Loc::Absent => {
                        st.blocked_unordered += 1;
                        push(&mut *st, "use-not-located");
                    }
                    Loc::Unordered => {
                        st.blocked_unordered += 1;
                        push(&mut *st, "unordered");
                    }
                    Loc::Place => {
                        st.blocked_place += 1;
                        push(&mut *st, "place");
                    }
                    // A PLACE use does not move the value, so inlining pulls
                    // its drop forward from the end of the block to the end
                    // of the consuming statement. That is unobservable only
                    // if the type certainly has no `Drop` -- which, with no
                    // type resolution, only the `let`'s own annotation can
                    // establish.
                    Loc::Found(_, true)
                        if !local_type(l).is_some_and(type_is_certainly_nondrop) =>
                    {
                        st.blocked_maybe_drop += 1;
                        push(&mut *st, "place-use-maybe-drop");
                    }
                    Loc::Found(p, _) => {
                        // Safe iff nothing observable happens before the use,
                        // OR the moved initializer is itself pure (so being
                        // skipped by, or reordered after, a panicking prefix
                        // is unobservable).
                        let free = p == Purity::Pure
                            || (p == Purity::MayPanic && init_purity == Purity::Pure);
                        if free {
                            st.free_temps += 1;
                            push(&mut *st, "free");
                        } else if p == Purity::Effectful {
                            st.blocked_effectful_prefix += 1;
                            push(&mut *st, "effectful-prefix");
                        } else {
                            st.blocked_panicking_prefix += 1;
                            push(&mut *st, "panicking-prefix");
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// JSON (hand-rolled: the probe must build offline from the registry cache the
// rest of the harness already has, and serde is not part of that contract)
// ---------------------------------------------------------------------------

fn jstr(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut st = Stats::default();
    let mut paths: Vec<String> = Vec::new();
    for a in &args {
        if a == "--stdin-list" {
            let mut buf = String::new();
            std::io::stdin().read_to_string(&mut buf).unwrap();
            paths.extend(buf.lines().map(|l| l.trim().to_string()).filter(|l| !l.is_empty()));
        } else {
            paths.push(a.clone());
        }
    }
    for p in &paths {
        match std::fs::read_to_string(p) {
            Ok(src) => analyze_file(p, &src, &mut st),
            Err(_) => {
                st.files += 1;
                st.parse_errors += 1;
            }
        }
    }

    let mut hist: Vec<(usize, usize)> = st.depth_hist.iter().map(|(k, v)| (*k, *v)).collect();
    hist.sort();
    let total: usize = hist.iter().map(|(_, n)| *n).sum();
    let mut p95 = 0usize;
    let mut acc = 0usize;
    for (d, n) in &hist {
        acc += n;
        if acc * 100 >= total * 95 {
            p95 = *d;
            break;
        }
    }
    let dmax = hist.last().map(|(d, _)| *d).unwrap_or(0);
    let mean = if total == 0 {
        0.0
    } else {
        hist.iter().map(|(d, n)| (*d * *n) as f64).sum::<f64>() / total as f64
    };

    let mut out = String::from("{\n");
    macro_rules! kv {
        ($k:expr, $v:expr) => {
            out.push_str(&format!("  {}: {},\n", jstr($k), $v));
        };
    }
    kv!("files", st.files);
    kv!("parse_errors", st.parse_errors);
    kv!("statements", st.statements);
    kv!("generated_lets", st.generated_lets);
    kv!("unread_generated_lets", st.unread_generated_lets);
    kv!("single_use_next_stmt", st.single_use_next_stmt);
    kv!("free_temps", st.free_temps);
    kv!("blocked_effectful_prefix", st.blocked_effectful_prefix);
    kv!("blocked_panicking_prefix", st.blocked_panicking_prefix);
    kv!("blocked_place", st.blocked_place);
    kv!("blocked_unordered", st.blocked_unordered);
    kv!("blocked_maybe_drop", st.blocked_maybe_drop);
    kv!("skipped_shadowed", st.skipped_shadowed);
    kv!("skipped_inline_fmt", st.skipped_inline_fmt);
    kv!("skipped_mut", st.skipped_mut);
    kv!("depth_max", dmax);
    kv!("depth_p95", p95);
    out.push_str(&format!("  {}: {:.4},\n", jstr("depth_mean"), mean));
    out.push_str(&format!("  {}: {{", jstr("depth_hist")));
    for (i, (d, n)) in hist.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("{}: {}", jstr(&d.to_string()), n));
    }
    out.push_str("},\n");
    out.push_str(&format!("  {}: [\n", jstr("sites")));
    for (i, s) in st.sites.iter().enumerate() {
        if i > 0 {
            out.push_str(",\n");
        }
        out.push_str(&format!(
            "    {{{}: {}, {}: {}, {}: {}, {}: {}, {}: {}}}",
            jstr("file"),
            jstr(&s.file),
            jstr("line"),
            s.line,
            jstr("name"),
            jstr(&s.name),
            jstr("verdict"),
            jstr(&s.verdict),
            jstr("text"),
            jstr(&s.text)
        ));
    }
    out.push_str("\n  ],\n");
    st.deep.sort_by(|a, b| b.depth.cmp(&a.depth));
    out.push_str(&format!("  {}: [\n", jstr("deep_sites")));
    for (i, d) in st.deep.iter().enumerate() {
        if i > 0 {
            out.push_str(",\n");
        }
        out.push_str(&format!(
            "    {{{}: {}, {}: {}, {}: {}, {}: {}}}",
            jstr("file"),
            jstr(&d.file),
            jstr("line"),
            d.line,
            jstr("depth"),
            d.depth,
            jstr("text"),
            jstr(&d.text)
        ));
    }
    out.push_str("\n  ]\n}\n");
    print!("{out}");
}
