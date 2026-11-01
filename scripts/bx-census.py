#!/usr/bin/env python3
"""bx-census: what keeps the local C++ corpus out of the emitrust-bx subset.

INTENT.  emitrust-bx (FR-250/FR-254) accepts a canonical C++17 subset and,
on anything else, prints ALL residuals (not first-failure) as
`file:line:col: error: <reason>` and exits 1.  This script turns those
residuals into a ranking instrument that obeys CLAUDE.md §3:

  * it counts PROGRAMS (one source file) and FUNCTIONS (one main-file
    function definition), never events; the raw event column is printed
    last and labelled as the least trustworthy;
  * its ranking column is a GREEDY SET COVER over per-program (and
    per-function) blocker sets -- marginal yield is a property of the SET,
    not of a tag's frequency;
  * every blocker set it reports is a LOWER BOUND and every yield an UPPER
    BOUND: emitrust-bx does not re-report cascades (uses of a rejected
    decl), does not descend into rejected constructs (a rejected class,
    namespace, template, or non-canonical `for` hides everything inside
    it), and the census cannot see what the next stage (rustc, the
    round-trip laws) would reject.  Supporting a tag can EXPOSE new tags.

Corpus (local only): every .cpp/.cc under
third_party/llvm-test-suite/SingleSource, test/Cpp17Suite/Inputs/*.cpp,
and test/CppStdSuite/candidates.txt (paths relative to the llvm-test-suite
root, as test/CppStdSuite/run_cppstd_suite.py resolves them).  Files are
deduplicated by realpath; a file that does not pass
`clang++ -std=c++17 -fsyntax-only` standalone is NOT_A_PROGRAM and is
reported separately, never mixed into the blocker tables.

Run inside the dev shell (emitrust-bx calls clang++ from PATH):

    nix develop -c python3 scripts/bx-census.py \
        --bx tools/emitrust-bx/emitrust-bx.py --out DIR -j 4
"""

import argparse
import collections
import json
import multiprocessing
import os
import random
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOWER_BOUND_BANNER = (
    "CAVEAT: every blocker set below is a LOWER BOUND and every yield an UPPER BOUND.\n"
    "emitrust-bx does not re-report cascades (uses of a rejected decl) and does not\n"
    "descend into rejected constructs (a rejected class/namespace/template hides every\n"
    "member; a non-canonical `for`/try hides its body; an expression residual such as\n"
    "`?:` or an overloaded-operator call does not inspect its operands; uses of a\n"
    "rejected local are silent). Supporting a tag WILL expose more tags.\n"
    "The corpus is test/benchmark code (llvm-test-suite, conformance inputs), NOT\n"
    "industrial code.\n")

# --------------------------------------------------------------------- corpus

def collect_corpus(repo):
    """[(abs path, [sources])], notes."""
    notes = []
    by_real = collections.OrderedDict()

    def add(p, src):
        r = os.path.realpath(p)
        by_real.setdefault(r, []).append(src)

    lts = os.path.join(repo, "third_party", "llvm-test-suite")
    ss = os.path.join(lts, "SingleSource")
    if not os.path.isdir(ss) or not os.listdir(ss):
        notes.append("llvm-test-suite submodule NOT checked out at %s; SingleSource "
                     "and candidates.txt contribute nothing" % lts)
    else:
        for root, dirs, files in os.walk(ss):
            dirs.sort()
            for f in sorted(files):
                if f.endswith((".cpp", ".cc")):
                    add(os.path.join(root, f), "llvm-test-suite/SingleSource")
    inp = os.path.join(repo, "test", "Cpp17Suite", "Inputs")
    if os.path.isdir(inp):
        for f in sorted(os.listdir(inp)):
            if f.endswith(".cpp"):
                add(os.path.join(inp, f), "Cpp17Suite/Inputs")
    cand = os.path.join(repo, "test", "CppStdSuite", "candidates.txt")
    if os.path.isfile(cand):
        with open(cand) as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                p = os.path.join(lts, line)
                if os.path.isfile(p):
                    add(p, "CppStdSuite/candidates")
                else:
                    notes.append("candidate missing on disk: %s" % line)
    return list(by_real.items()), notes


# --------------------------------------------------------------------- tags

KTY = r"(?:i8|i16|i32|i64|u8|u16|u32|u64|usize|isize|f64|f32|bool|unit|str)"
BUILTIN = {
    "int", "long", "long long", "short", "char", "signed char", "unsigned char",
    "unsigned", "unsigned int", "unsigned long", "unsigned long long", "unsigned short",
    "float", "double", "long double", "bool", "void", "wchar_t", "char16_t", "char32_t",
    "size_t", "std::size_t", "__int128", "unsigned __int128", "nullptr_t", "std::nullptr_t",
}


def _strip_targs(s):
    out, depth = [], 0
    for ch in s:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def type_sub(spelled):
    """'const std::vector<int> &' -> 'type:std::vector&'; user types -> type:<user>."""
    s = (spelled or "").strip()
    if "(" in s:
        return "type:<function/function-pointer>"
    flat = _strip_targs(s)
    shape = ""
    if "[" in flat:
        shape = "[]"
    elif flat.rstrip().endswith("&&"):
        shape = "&&"
    elif flat.rstrip().endswith("&"):
        shape = "&"
    elif "*" in flat:
        shape = "*"
    base = re.sub(r"\[[^\]]*\]", "", flat)
    base = base.replace("*", " ").replace("&", " ")
    base = re.sub(r"\b(const|volatile|struct|class|union|enum|typename|restrict|__restrict)\b", " ", base)
    base = re.sub(r"\s+", " ", base).strip()
    if base.startswith("::"):
        base = base[2:]
    if base in BUILTIN:
        pass
    elif base.startswith("std::") or base.startswith("__gnu_cxx::"):
        base = "::".join(base.split("::")[:2])  # std::vector, std::basic_string
    elif base.startswith("type-parameter") or base == "auto":
        base = "<dependent>"
    else:
        base = "<user>"
    return "type:" + base + shape


def generic(reason):
    s = re.sub(r"'[^']*'", "'_'", reason)
    s = re.sub(r"`[^`]*`", "`_`", s)
    s = re.sub(r"\b\d+\b", "N", s)
    s = re.sub(r"\b%s\b" % KTY, "T", s)
    return s


# (regex on the reason with any leading "unsupported: " removed, TAG, SUB-KIND)
# SUB-KIND: None | "type" (group 1 is a C++ type spelling) | "libfn" | "op" |
#           "cast" | "kty" | "init" | "generic" (sub = generic-normalized text)
RULES = [
    # ---- importer (cxx_import.py) -----------------------------------------
    (r"^call to library function '(.*)'$", "call to library function", "libfn"),
    (r"^parameter of type '(.*)'$", "parameter of non-kernel type", "type"),
    (r"^return type '(.*)'$", "return of non-kernel type", "type"),
    (r"^variable of type '(.*)'$", "local variable of non-kernel type", "type"),
    (r"^conversion to '(.*)'$", "conversion to non-kernel type", "type"),
    (r"^cast to '(.*)'$", "explicit cast to non-kernel type", "type"),
    (r"^integer literal of type '(.*)'$", "literal of non-kernel type", "type"),
    (r"^character literal of type '(.*)'$", "literal of non-kernel type", "type"),
    (r"^floating literal of type '(.*)'$", "literal of non-kernel type", "type"),
    (r"^compound assignment computed in type '(.*)'$", "compound assignment in non-kernel type", "type"),
    (r"^printf argument of type '(.*)' for '.*'$", "printf argument of non-kernel type", "type"),
    (r"^operator '(.*)'$", "operator outside the subset", "op"),
    (r"^unary operator '(.*)'$", "unary operator outside the subset", "op"),
    (r"^implicit conversion \((.*)\)$", "implicit conversion", "cast"),
    (r"^(.*)-initialization \(the subset spells it", "non-copy initialization", "init"),
    (r"^reference to (.*)$", None, None),  # tag = whole reason
    (r"^local (.*)$", None, None),
    (r"^(static|extern) (function|local variable)$", None, None),
    (r"^printf with \d+ conversions and \d+ arguments$", "printf conversion/argument count mismatch", None),
    (r"^printf", "printf format feature", "generic"),
    (r"^print! ", "printf format feature", "generic"),
    (r"^string (literal with an encoding prefix|escape)", "string literal feature", "generic"),
    (r"^`for` loop outside the canonical counting form", "non-canonical for loop", None),
    (r"^return inside main other than the final", None, None),
    (r"^(if with an init-statement or condition variable|if constexpr|while with a condition variable"
     r"|assignment to something other than a local variable|expression statement without effect"
     r"|compound assignment to something other than a local variable|compound assignment to bool"
     r"|increment of something other than a local variable|increment of a bool|call"
     r"|auto-typed declaration|decltype-typed declaration|constexpr variable"
     r"|declaration without an initializer|string literal outside a printf argument"
     r"|array-to-pointer decay|function pointer|cast to void|non-ASCII character literal"
     r"|floating literal|non-finite floating literal|global variable|enumerator"
     r"|function used as a value|increment/decrement used as a value|address-of|dereference"
     r"|assignment used as a value|comma operator|compound assignment used as a value"
     r"|printf result used as a value|indirect call|default argument|unnamed parameter"
     r"|inline function|constexpr function|variadic function|exception specification"
     r"|main not returning int|function declared but not defined|extern \"C\" declaration"
     r"|using-directive)$", None, None),
    # FRIENDLY table values (whole-construct rejections)
    (r"^(array subscript|member access|member function call|lambda expression|new-expression"
     r"|delete-expression|sizeof/alignof|conditional operator `\?:`|this|nullptr"
     r"|initializer list|object construction|overloaded operator call|throw-expression"
     r"|switch statement|do-while loop|goto|label|try block|range-based for loop"
     r"|class/struct definition|namespace|function template|class template|typedef"
     r"|type alias|enum|using-declaration|static_assert|member function|compound literal"
     r"|statement expression)$", None, None),
    # FR-260 namespace rejections: the first three reject the namespace WHOLE (OPAQUE)
    (r"^inline namespace$", "namespace: inline", None),
    (r"^named namespace inside an anonymous namespace$", "namespace: named inside anonymous", None),
    (r"^namespace name: ", "namespace: name not representable", None),
    (r"^function 'main' inside a namespace$", "namespace: main inside", None),
    (r"^namespace alias of a namespace outside this file$", "namespace: foreign alias", None),
    (r"^'.*' is internal to namespace '.*'", "namespace: internal member referenced outside", None),
    (r"^function '.*' has the name of a namespace", "namespace: function named like a namespace", None),
    # FR-257 struct/class rejections. "union" and the unnamed-record "class/struct
    # definition" reject the record WHOLE (OPAQUE); every other class-level reason
    # is reported while bx still inspects the member bodies.
    (r"^union$", "struct: union", None),
    (r"^class with a base class", "struct: inheritance", None),
    (r"^struct with members outside the subset$", "struct: has members outside the subset", None),
    (r"^member function of a struct with members outside the subset$", "struct: has members outside the subset", None),
    (r"^definition of a rejected member function$", "struct: definition of a rejected member", None),
    (r"^(private or protected member|bit-field|default member initializer|mutable data member"
     r"|unnamed data member|const data member|static data member|operator overload"
     r"|virtual member function|defaulted or deleted member function|volatile member function"
     r"|user-declared copy or move constructor|overloaded constructor|user-declared destructor"
     r"|friend declaration|member function template|nested class|enum inside a class"
     r"|member typedef|static_assert inside a class|using-declaration inside a class"
     r"|anonymous union or struct member|conversion operator|member function declared but not defined"
     r"|use of a rejected data member|use of a rejected member function|member function used as a value"
     r"|member access on a temporary|member function call on a temporary"
     r"|static member function called through an object"
     r"|aggregate initialization that omits data members"
     r"|aggregate initialization of a struct with rejected data members"
     r"|struct variable without an initializer"
     r"|delegating or base-class constructor initializer)\b", "struct: member", "generic"),
    (r"^data member of type '(.*)'$", "struct: data member of non-kernel type", "type"),
    (r"^.* inside a class$", "struct: member", "generic"),
    (r"^value-initialized or default-initialized struct", "struct: default/value initialization", None),
    (r"^(constructor whose body|constructor of '.*' whose body) is not a member-initializer",
     "struct: constructor body", None),
    (r"^aggregate initialization of '.*', which has a constructor", "struct: aggregate init of a struct with a constructor", None),
    (r"^move out of a data member", "struct: move out of a data member", None),
    (r"^member function call on something other than", "struct: call receiver form", None),
    (r"^(an argument of '.*' modifies '.*', the object"
     r"|'.*' is passed by reference to '.*', a non-const member function"
     r"|non-const member function '.*' is called on an element of"
     r"|non-const member function '.*' is called on '.*', which is also used elsewhere)",
     "struct: receiver aliasing", None),
    (r"^constructor call evaluated only for its effect$", "struct: constructor call for effect", None),
    (r"^call of non-const member function '.*' of immutable", "struct: mutating call on immutable", None),
    (r"^(member function '.*' called without an object|static member function '.*' called on an object"
     r"|constructor '.*' called on an object)", "struct: call form", None),
    (r"^(struct name: |struct '.*' has the name of|variable '.*' has the name of a struct"
     r"|data member '.*' has the name of a struct|member function '.*' has the name of a struct)",
     "struct: name clash", None),
    # FR-253 <algorithm> / <numeric> (before FR-252: `callable parameter '.*' passed to std::`)
    (r"^std::\w+ over something other than a whole container", "algorithm: free iterators (normalizer)", None),
    (r"^std::\w+ over the begin\(\) and end\(\) of two different", "algorithm: range over two containers", None),
    (r"^std::\w+ over something other than a variable", "algorithm: range receiver form", None),
    (r"^std::\w+ over a (std::string|container that is not a std::vector)", "algorithm: non-vector container", None),
    (r"^lambda expression as an argument of std::", "algorithm: inline lambda (normalizer)", None),
    (r"^(function '.*' passed to std::|the callable argument of std::|callable parameter '.*' passed to std::)",
     "algorithm: callable argument form", None),
    (r"^(std::stable_sort with a comparator|std::transform over two ranges|std::\w+ with (a comparator|\d+ arguments)"
     r"|std::accumulate takes)",
     "algorithm: overload outside the subset", "generic"),
    (r"^std::transform into ", "algorithm: transform destination form", None),
    (r"^(std::\w+ used as a value|std::\w+ other than `|std::find compared with)",
     "algorithm: iterator result (normalizer)", "generic"),
    (r"^(std::max_element over a vector of double|std::(max|min) of two doubles|std::(stable_)?sort of a vector of double)",
     "algorithm: f64 has no total order", None),
    (r"^std::\w+ of a value of type .* in a vector of", "algorithm: value/element type mismatch (normalizer)", None),
    (r"^std::accumulate of .* elements from an initial value", "algorithm: accumulate init type (normalizer)", None),
    (r"^(std::\w+ (over|of) an? |std::\w+ with a comparator over|std::accumulate with an operation over"
     r"|std::transform into a std::back_inserter over|std::(max|min) of two values)",
     "algorithm: element type outside the subset", "generic"),
    (r"^closure '.*' has type .*, but std::", "algorithm: closure signature mismatch", None),
    (r"^closure '.*' modifies its own copy of a capture \(a `mutable` lambda\) and is passed to std::",
     "algorithm: mutable lambda copied (normalizer)", None),
    (r"^(the value of std::fill reads|std::transform appends to)", "algorithm: reads its own target (borrow)", None),
    (r"^(operands of std::max have different types|std::\w+ of immutable)", "algorithm: typing", "generic"),
    (r"^(closure argument '.*' of std::|an argument of std::|the fold of std::accumulate|fold closure other than"
     r"|iterator method `|`\.(max|min|map|extend|contains)\(\)|`v\.iter\(\)|`bx::|`std::cmp::)",
     "algorithm: Rust spelling", "generic"),
    # FR-252 closures
    (r"^default capture `","closure: default capture (normalizer)", None),
    (r"^mixed by-copy and by-reference captures", "closure: mixed captures (normalizer)", None),
    (r"^init-capture `", "closure: init-capture (normalizer)", None),
    (r"^(capture of `this`|capture pack|closure '.*' captures `(this|self)`)", "closure: captures this/pack", None),
    (r"^(generic lambda|function template instantiated with a generic lambda)", "closure: generic lambda", None),
    (r"^(lambda inside a lambda body|closure '.*' defined inside closure)", "closure: nested", None),
    (r"^(copy of a closure|closure '.*' used other than by calling it)", "closure: copied or used as a value", None),
    (r"^(closure returning|closure '.*' returning|closure parameter|unnamed closure parameter|closure body that is not a block)",
     "closure: signature outside the subset", "generic"),
    (r"^'.*' is (used|modified or moved) while closure '.*', which (modifies it through a by-reference capture|captures it by reference)",
     "closure: by-reference capture borrow conflict", None),
    (r"^('.*' is used after closure '.*' captures it by copy|closure '.*' captures '.*' by copy inside a loop)",
     "closure: by-copy capture then use (Rust move)", None),
    (r"^closure '.*', which modifies state, is called twice in one statement", "closure: unsequenced double call", None),
    (r"^closure '.*' (captures the pointer|captures the reference|uses the callable)", "closure: capture of a pointer/reference/callable", "generic"),
    (r"^closure '.*' declares '.*', which names a variable of the enclosing scope", "closure: capture shadowed by a local", None),
    (r"^closure '.*' moves its capture", "closure: moves its capture out (FnOnce)", None),
    (r"^function template", "function template", "generic"),
    (r"^(callable parameter|parameter of type '.*' in a function template|argument \d+ of '.*' is a callable"
     r"|closure '.*' modifies state but is passed to a|function '.*' with (more than one callable parameter|a callable parameter)"
     r"|member function '.*' with a callable parameter|`&(mut )?impl Fn)", "closure: callable parameter form", "generic"),
    (r"^(closure '.*' (is not spelled the canonical way|may reach its end|modifies its capture of immutable)"
     r"|call to void closure|call of a callable object other than)", "closure: form", "generic"),
    (r"^the body of a range-for over '.*' modifies", "range-for: body modifies its container", None),
    # FR-263
    (r"^two calls that may print in one statement", "sequencing: two printing calls (normalizer)", None),
    (r"^identifier '.*' is reserved by the canonical Rust spelling", "identifier: reserved by canonical spelling", None),
    (r"^struct '.*' contains itself by value", "struct: contains itself", None),
    (r"^member access through a pointer$", "member access through a pointer", None),
    (r"^use of a rejected declaration$", "cascade: use of a rejected declaration", None),
    # FR-261 pointers <=> suffix slices
    (r"^(signed pointer offset|pointer arithmetic `p - k`|pointer decrement)", "pointer: backwards (R3)", None),
    (r"^(pointer comparison|pointer difference)", "pointer: compare/subtract (R4)", None),
    (r"^null pointer", "pointer: null (R6)", None),
    (r"^(pointer arithmetic other than|pointer offset of type|pointer arithmetic on something other)",
     "pointer: arithmetic form", "generic"),
    (r"^address of a pointer", "pointer: pointer to pointer (R6)", None),
    (r"^(reinterpret_cast|const_cast)$", None, None),
    (r"^std::vector::data of something other than", "pointer: data() form", None),
    (r"^'.*' is passed to '.*' as a mutable pointer", "pointer: argument aliasing (R1)", None),
    (r"^('.*' is used while '.*', a mutable pointer derived|'.*' is modified or moved while '.*', a pointer into it)",
     "pointer: root used while borrowed (R2)", None),
    (r"^a mutable slice of '.*' starts at an offset that reads", "pointer: re-slice reads its root", None),
    (r"^(assignment of a pointer|.* is a pointer other than|indexing a pointer expression"
     r"|address of something other than|address of an element in the Kernel)", "pointer: expression form", "generic"),
    (r"^.* through '.*', a pointer to const$", "pointer: write through const", None),
    (r"^function returning a pointer$", "pointer: returned (R6)", None),
    # FR-259 cheap syntax
    (r"^conditional operator `\?:` of type '(.*)' \(", "?: of non-scalar type", "type"),
    (r"^conditional operator `\?:` used as an lvalue$", "?: used as an lvalue", None),
    (r"^increment/decrement of '.*' used as a value in a statement that also uses", "incdec: unsequenced", None),
    (r"^unsigned compound assignment to an element whose index increments", "incdec: index double evaluation", None),
    (r"^increment/decrement of (.*) used as a value$", "incdec: non-integer target", "generic"),
    (r"^(increment of immutable|increment block)", "incdec: form", "generic"),
    (r"^(condition of a conditional expression|arms of a conditional expression|conditional expression of type)",
     "typing: conditional expression", "generic"),
    (r"^identifier '.*' is reserved by the canonical Rust spelling", "identifier: reserved by canonical spelling", None),
    # clang kinds with no FRIENDLY name (friendly() falls back to the kind)
    (r"^([A-Z][A-Za-z]*(Expr|Stmt|Decl|Operator|Literal))$", None, None),
    # ---- typecheck.py ------------------------------------------------------
    (r"^unsigned `(.*)=` \(panics", "unsigned compound arithmetic (overflow panics)", "op"),
    (r"^unsigned `(.*)` \(panics", "unsigned arithmetic (overflow panics)", "op"),
    (r"^`\.wrapping_(\w+)` on", "signed wrapping arithmetic", "op"),
    (r"^argument \d+ of '.*' has type", "typing: argument type mismatch", None),
    (r"^'.*' takes \d+ arguments, \d+ given$", "typing: argument count mismatch", None),
    (r"^expression has type \S+, expected", "typing: expression type mismatch", None),
    (r"^operands of .* have different types", "typing: mixed-type operands", None),
    (r"^function '.*' defined twice$", "function defined twice (overloading)", None),
    (r"^function '.*' may reach its end without returning", "typing: may fall off end of non-void function", None),
    (r"^redeclaration of '.*' in the same scope$", "typing: shadowing redeclaration in same scope", None),
    (r"^variable '.*' has the name of a function$", "typing: variable named like a function", None),
    (r"^initializer of '.*' refers to a variable of the same name$", "typing: self-referential initializer name", None),
    (r"^use of undeclared variable", "typing: use of undeclared variable (cascade)", None),
    (r"^call to undeclared function", "typing: call to undeclared function (cascade)", None),
    (r"^assignment to undeclared", "typing: assignment to undeclared (cascade)", None),
    (r"^assignment to immutable", "typing: assignment to immutable", None),
    (r"^call to void function '.*' used as a value$", "typing: void call used as a value", None),
    (r"^(return with a value in a void function|return without a value in a function returning)",
     "typing: return form", None),
    (r"^counting loop", "typing: counting-loop canonical form", "generic"),
    (r"^integer literal -?\d+ out of range for", "typing: integer literal out of range", None),
    (r"^(break|continue) outside a loop$", "typing: break/continue outside loop", None),
    (r"^(arithmetic|integer op|comparison of|negation of|'not' of|logical|shift|compound shift on"
     r"|compound \S+ on|cast from|cast to|parameter type|return type|variable type)\b",
     "typing: operation on kernel type", "generic"),
    (r"^(main may not contain return|main must return unit|main with parameters|call to main)$", None, None),
    (r"^(float literal|malformed float literal|integer literal of non-integer type)", "typing: literal form", "generic"),
    (r"^duplicate parameter", "typing: duplicate parameter", None),
    (r"^expression statement that is not a call$", None, None),
    (r"^identifier '(operator\W.*)' is not", "operator overload definition", "op"),
    (r"^identifier '.*' (is not ASCII|is not a plain identifier)", "identifier: not a plain identifier", None),
    (r"^identifier '.*' starts with an underscore", "identifier: leading underscore", None),
    (r"^identifier '.*' contains a double underscore", "identifier: double underscore", None),
    (r"^identifier '.*' is a Rust keyword", "identifier: Rust keyword", None),
    (r"^identifier '.*' is a C\+\+ keyword", "identifier: C++ keyword", None),
    (r"^identifier '.*' is reserved by the canonical spelling", "identifier: reserved by canonical spelling", None),
]
RULES = [(re.compile(p), t, k) for p, t, k in RULES]

# Tags whose construct emitrust-bx rejects WHOLE without descending: everything
# inside is invisible, so a greedy step that picks one of these is the most
# optimistic row in the table (the hidden contents will surface new tags).
OPAQUE = {
    "try block", "non-canonical for loop", "switch statement", "do-while loop",
    "range-based for loop", "if with an init-statement or condition variable", "if constexpr",
    "while with a condition variable", "lambda expression", "class/struct definition",
    "namespace", "function template", "class template", "member function",
    "CXXConstructorDecl", "CXXDestructorDecl", "CXXConversionDecl",
    "ClassTemplatePartialSpecializationDecl", "ClassTemplateSpecializationDecl",
    "local class/struct definition", "label", "statement expression", "GCCAsmStmt",
    "namespace: inline", "namespace: named inside anonymous", "namespace: name not representable",
    "struct: union",
}

# a member-function definition bx rejects as ONE item without inspecting its body
WHOLE_MEMBER = {"member function", "CXXConstructorDecl", "CXXDestructorDecl", "CXXConversionDecl",
                "struct: definition of a rejected member"}


def mark(key):
    tag = key.split(" [", 1)[0]
    return key + ("  *OPAQUE" if tag in OPAQUE else "")


def classify(raw_reason):
    """(tag, sub or None, matched: bool)."""
    r = raw_reason
    if r.startswith("unsupported: "):
        r = r[len("unsupported: "):]
    for rx, tag, kind in RULES:
        m = rx.search(r)
        if not m:
            continue
        if tag is None:
            tag = r
        sub = None
        if kind == "type":
            sub = type_sub(m.group(1))
        elif kind == "libfn":
            sub = "libfn:" + m.group(1)
        elif kind == "op":
            sub = "op:" + m.group(1)
        elif kind == "cast":
            sub = "cast:" + m.group(1)
        elif kind == "init":
            sub = "init:" + m.group(1)
        elif kind == "generic":
            sub = "form:" + generic(r)
        return tag, sub, True
    return "other: " + generic(r), None, False


# --------------------------------------------------------------------- clang AST

FUNC_KINDS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl",
              "CXXConversionDecl"}
CONTAINER_KINDS = {"NamespaceDecl", "LinkageSpecDecl", "CXXRecordDecl", "ClassTemplateDecl",
                   "ClassTemplateSpecializationDecl", "ClassTemplatePartialSpecializationDecl",
                   "FunctionTemplateDecl", "ExportDecl"}


def _off(lo):
    if not isinstance(lo, dict):
        return None
    if "expansionLoc" in lo:
        return lo["expansionLoc"].get("offset")
    return lo.get("offset")


class FileTracker:
    """Replays clang's JSON print order to know each node's file (as emitrust-bx does)."""

    def __init__(self):
        self.cur = None

    def loc(self, lo):
        if not isinstance(lo, dict):
            return
        if "spellingLoc" in lo or "expansionLoc" in lo:
            self.loc(lo.get("spellingLoc"))
            self.loc(lo.get("expansionLoc"))
            return
        f = lo.get("file")
        if f is not None:
            self.cur = f

    def walk(self, n):
        stack = [n]
        while stack:
            x = stack.pop()
            if isinstance(x, list):
                stack.extend(reversed(x))
                continue
            if not isinstance(x, dict):
                continue
            if isinstance(x.get("loc"), dict):
                self.loc(x["loc"])
            rg = x.get("range")
            if isinstance(rg, dict):
                self.loc(rg.get("begin"))
                self.loc(rg.get("end"))
            kids = [v for k, v in x.items()
                    if k not in ("loc", "range", "type", "referencedDecl", "foundReferencedDecl")
                    and isinstance(v, (list, dict))]
            stack.extend(reversed(kids))


def callee_names(body):
    out = set()
    stack = [body]
    while stack:
        x = stack.pop()
        if isinstance(x, list):
            stack.extend(x)
            continue
        if not isinstance(x, dict):
            continue
        k = x.get("kind")
        if k == "DeclRefExpr":
            rd = x.get("referencedDecl") or {}
            if rd.get("kind") in FUNC_KINDS and rd.get("name"):
                out.add(rd["name"])
        elif k == "MemberExpr" and x.get("name"):
            out.add(x["name"])
        elif k in ("CXXConstructExpr", "CXXTemporaryObjectExpr"):
            cd = x.get("ctorDecl") or {}
            if cd.get("name"):
                out.add(cd["name"])
        for kk, v in x.items():
            if kk in ("type", "referencedDecl", "foundReferencedDecl", "range", "loc"):
                continue
            if isinstance(v, (list, dict)):
                stack.append(v)
    return out


def main_file_functions(ast, path):
    """Function definitions in the main file: dicts with range offsets and container ancestry."""
    real = os.path.realpath(path)
    rcache = {}

    def is_main(f):
        if f is None:
            return False
        r = rcache.get(f)
        if r is None:
            r = rcache[f] = os.path.realpath(f)
        return r == real

    trk = FileTracker()
    funcs = {}

    def visit(n, ancestors, qual):
        k = n.get("kind")
        if n.get("isImplicit"):
            return
        rg = n.get("range") or {}
        b, e = _off(rg.get("begin")), _off(rg.get("end"))
        if k in FUNC_KINDS:
            body = [c for c in n.get("inner", []) if isinstance(c, dict) and c.get("kind") == "CompoundStmt"]
            if not body or b is None or e is None:
                return
            key = (b, e)
            if key in funcs:  # template instantiations share the pattern's range
                return
            name = n.get("name") or "<anon>"
            funcs[key] = {
                "name": "::".join(qual + [name]),
                "short": name,
                "kind": k,
                "begin": b, "end": e,
                "containers": [(a[0], a[1]) for a in ancestors],  # (kind, begin offset)
                "callees": sorted(callee_names(body[-1])),
            }
            return
        if k in CONTAINER_KINDS:
            nq = qual + [n["name"]] if n.get("name") and k in (
                "NamespaceDecl", "CXXRecordDecl", "ClassTemplateSpecializationDecl",
                "ClassTemplatePartialSpecializationDecl") else qual
            na = ancestors + [(k, b)]
            for c in n.get("inner", []) or []:
                if isinstance(c, dict):
                    visit(c, na, nq)

    for n in ast.get("inner", []):
        trk.loc(n.get("loc"))
        main = is_main(trk.cur)
        rest = {k: v for k, v in n.items() if k != "loc"}
        trk.walk(rest)
        if main:
            visit(n, [], [])
    return sorted(funcs.values(), key=lambda f: f["begin"])


# --------------------------------------------------------------------- worker

DIAG_RX = re.compile(r"^(?P<path>.*?):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.*)$")


def line_starts(data):
    st = [0]
    for i, b in enumerate(data):
        if b == 0x0A:
            st.append(i + 1)
    return st


def analyze(job):
    path, bx, clang, timeout = job
    rec = {"path": path, "status": None}
    t0 = time.time()
    try:
        p = subprocess.run([clang, "-std=c++17", "-fsyntax-only", "-Xclang", "-ast-dump=json", path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        rec["status"] = "TIMEOUT"
        rec["detail"] = "clang ast dump > %ss" % timeout
        return rec
    if p.returncode != 0:
        rec["status"] = "NOT_A_PROGRAM"
        errs = [l for l in p.stderr.decode("utf-8", "replace").splitlines() if ": error: " in l]
        rec["detail"] = errs[0] if errs else "clang exit %d" % p.returncode
        return rec
    try:
        ast = json.loads(p.stdout)
        del p
        funcs = main_file_functions(ast, path)
        del ast
    except Exception as ex:  # noqa: BLE001 -- census must keep going, and say so
        rec["status"] = "CENSUS_ERROR"
        rec["detail"] = "AST walk: %r" % ex
        return rec
    try:
        b = subprocess.run([sys.executable, bx, "--emit=kernel", path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        rec["status"] = "TIMEOUT"
        rec["detail"] = "emitrust-bx > %ss" % timeout
        return rec
    with open(path, "rb") as fh:
        starts = line_starts(fh.read())
    diags, unparsed = [], []
    for line in b.stderr.decode("utf-8", "replace").splitlines():
        m = DIAG_RX.match(line)
        if m and os.path.realpath(m.group("path")) == os.path.realpath(path):
            ln, col = int(m.group("line")), int(m.group("col"))
            off = starts[ln - 1] + col - 1 if 0 < ln <= len(starts) else None
            tag, sub, matched = classify(m.group("msg"))
            diags.append({"line": ln, "col": col, "off": off, "reason": m.group("msg"),
                          "tag": tag, "sub": sub, "matched": matched})
        elif line.strip():
            unparsed.append(line)
    if b.returncode == 0:
        rec["status"] = "IN_SUBSET"
    elif diags:
        rec["status"] = "BLOCKED"
    else:
        rec["status"] = "BX_CRASH"
        rec["detail"] = "\n".join(unparsed[-5:])
    if unparsed and diags:
        rec["unparsed_stderr"] = unparsed[:5]
    # attribute diagnostics to functions
    for f in funcs:
        f["diags"] = []
    for i, d in enumerate(diags):
        d["func"] = None
        if d["off"] is None:
            continue
        inner = None
        for fi, f in enumerate(funcs):
            if f["begin"] <= d["off"] <= f["end"]:
                if inner is None or f["begin"] >= funcs[inner]["begin"]:
                    inner = fi
        if inner is not None:
            funcs[inner]["diags"].append(i)
            d["func"] = funcs[inner]["name"]
    # container rejections (bx rejects a class/namespace/template as ONE item
    # at its begin location and never looks inside): every member inherits it.
    by_off = collections.defaultdict(list)
    for i, d in enumerate(diags):
        by_off[d["off"]].append(i)
    for f in funcs:
        f["container_diags"] = sorted({i for _, cb in f["containers"] for i in by_off.get(cb, [])
                                       if diags[i]["func"] is None})
    rec["diags"] = diags
    rec["functions"] = [{"name": f["name"], "short": f["short"], "kind": f["kind"],
                         "callees": f["callees"],
                         "diags": f["diags"], "container_diags": f["container_diags"]}
                        for f in funcs]
    rec["secs"] = round(time.time() - t0, 2)
    return rec


# --------------------------------------------------------------------- analysis

def greedy_cover(sets, steps, prog_of=None):
    """sets: {item: frozenset(keys)} (empty = already clean). Greedy: pick the key that
    unblocks the most items given every key already chosen; tie-break / zero-gain
    fallback on fractional weight sum(1/|remaining|)."""
    chosen = set()
    remaining = {i: set(s) for i, s in sets.items() if s}
    rows = []
    cum = sum(1 for s in sets.values() if not s)
    for _ in range(steps):
        if not remaining:
            break
        gain = collections.Counter()
        frac = collections.defaultdict(float)
        for i, s in remaining.items():
            if len(s) == 1:
                gain[next(iter(s))] += 1
            for k in s:
                frac[k] += 1.0 / len(s)
        best = max(frac, key=lambda k: (gain[k], frac[k], k))
        chosen.add(best)
        unblocked = [i for i, s in remaining.items() if s <= {best}]
        for i in list(remaining):
            remaining[i].discard(best)
            if not remaining[i]:
                del remaining[i]
        cum += len(unblocked)
        row = {"key": best, "marginal": len(unblocked), "frac_weight": round(frac[best], 2),
               "cumulative": cum}
        if prog_of is not None:
            row["programs"] = len({prog_of(i) for i in unblocked})
        rows.append(row)
    return rows


def build(records, corpus_sources, args):
    prog_tags, prog_subs = {}, {}
    func_tags, func_subs = {}, {}
    tag_events, sub_events = collections.Counter(), collections.Counter()
    other_events = collections.Counter()
    func_short = {}
    for r in records:
        if r["status"] not in ("IN_SUBSET", "BLOCKED"):
            continue
        P = os.path.relpath(r["path"], REPO)
        ds = r.get("diags", [])
        prog_tags[P] = frozenset(d["tag"] for d in ds)
        prog_subs[P] = frozenset(d["tag"] + (" [" + d["sub"] + "]" if d["sub"] else "") for d in ds)
        for d in ds:
            tag_events[d["tag"]] += 1
            sub_events[d["tag"] + (" [" + d["sub"] + "]" if d["sub"] else "")] += 1
            if not d["matched"]:
                other_events[d["reason"]] += 1
        for fi, f in enumerate(r.get("functions", [])):
            F = "%s::%s#%d" % (P, f["name"], fi)  # overloads share a name
            idx = f["diags"] + f["container_diags"]
            func_tags[F] = frozenset(ds[i]["tag"] for i in idx)
            func_subs[F] = frozenset(ds[i]["tag"] + (" [" + ds[i]["sub"] + "]" if ds[i]["sub"] else "")
                                     for i in idx)
            # HIDDEN: bx rejected an enclosing item whole and never inspected this body --
            # either an enclosing class/namespace/template, or this function is itself a
            # top-level non-FunctionDecl item (out-of-line method/ctor/dtor), which bx's
            # run() rejects as one ResidualItem without descending.
            # Since FR-257 bx inspects members of a named struct and out-of-line member
            # bodies, so only an OPAQUE container tag or a whole-item member tag hides.
            hidden = any(ds[i]["tag"] in OPAQUE for i in f["container_diags"]) or (
                f["kind"] != "FunctionDecl" and any(ds[i]["tag"] in WHOLE_MEMBER for i in f["diags"]))
            func_short[F] = (P, f["short"], f["callees"], hidden, bool(f["diags"]))
    # transitive cleanliness over an APPROXIMATE, name-based, same-file call graph
    clean = {F for F, s in func_tags.items() if not s}
    by_name = collections.defaultdict(set)
    for F, (P, short, _, _, _) in func_short.items():
        by_name[(P, short)].add(F)
    trans = set(clean)
    changed = True
    while changed:
        changed = False
        for F in list(trans):
            P, _, callees, _, _ = func_short[F]
            for c in callees:
                tgt = by_name.get((P, c), set())
                if tgt and not tgt <= trans:
                    trans.discard(F)
                    changed = True
                    break
    return {
        "prog_tags": prog_tags, "prog_subs": prog_subs, "func_tags": func_tags,
        "func_subs": func_subs, "tag_events": tag_events, "sub_events": sub_events,
        "other_events": other_events, "func_short": func_short, "trans_clean": trans,
    }


def table_counts(prog_sets, func_sets, events):
    rows = []
    keys = set(events)
    for k in keys:
        np_ = sum(1 for s in prog_sets.values() if k in s)
        sole_p = sum(1 for s in prog_sets.values() if s == {k})
        nf = sum(1 for s in func_sets.values() if k in s)
        sole_f = sum(1 for s in func_sets.values() if s == {k})
        rows.append((k, np_, sole_p, nf, sole_f, events[k]))
    rows.sort(key=lambda r: (-r[1], -r[3], r[0]))
    return rows


def render(records, corpus, notes, B, args):
    out = []
    w = out.append
    w("emitrust-bx census (bx = %s)" % args.bx)
    w("=" * 78)
    w(LOWER_BOUND_BANNER)
    w("*OPAQUE marks a tag whose construct bx rejects whole WITHOUT inspecting its contents;"
      " its yield is the most optimistic in any table.\n")
    for n in notes:
        w("NOTE: " + n)
    # corpus by source
    st_by_src = collections.defaultdict(collections.Counter)
    srcmap = {p: s for p, s in corpus}
    for r in records:
        for s in sorted(set(srcmap[r["path"]])):
            st_by_src[s][r["status"]] += 1
        st_by_src["ALL (dedup by realpath)"][r["status"]] += 1
    statuses = ["IN_SUBSET", "BLOCKED", "NOT_A_PROGRAM", "TIMEOUT", "BX_CRASH", "CENSUS_ERROR"]
    w("\n(0) CORPUS -- one PROGRAM = one source file; a file in two sources counts in each row")
    w("%-34s %6s " % ("source", "files") + " ".join("%13s" % s for s in statuses))
    for s in sorted(st_by_src, key=lambda x: (x.startswith("ALL"), x)):
        c = st_by_src[s]
        w("%-34s %6d " % (s, sum(c.values())) + " ".join("%13d" % c[x] for x in statuses))
    bad = [r for r in records if r["status"] in ("TIMEOUT", "BX_CRASH", "CENSUS_ERROR")]
    for r in bad:
        w("  %s %s: %s" % (r["status"], os.path.relpath(r["path"], REPO), (r.get("detail") or "")[:160]))

    pt, ft = B["prog_tags"], B["func_tags"]
    nprog = len(pt)
    nclean = sum(1 for s in pt.values() if not s)
    nf = len(ft)
    fclean = sum(1 for s in ft.values() if not s)
    only_container = sum(1 for F, s in ft.items() if s and not B["func_short"][F][4])
    w("\n(a) FULLY INSIDE THE SUBSET")
    w("  programs analyzed: %d   in subset: %d" % (nprog, nclean))
    w("  main-file function definitions: %d   no residual of their own: %d" % (nf, fclean))
    w("  ... and transitively clean (every same-file callee also clean; name-based, APPROXIMATE): %d"
      % len(B["trans_clean"]))
    w("  functions whose ONLY blockers come from an enclosing rejected class/namespace/template: %d"
      % only_container)
    w("  OBSERVED functions (bx actually inspected the body): %d, of which clean: %d"
      % (sum(1 for F in ft if not B["func_short"][F][3]),
         sum(1 for F, s in ft.items() if not B["func_short"][F][3] and not s)))

    conc = collections.Counter(B["func_short"][F][0] for F in ft)
    hid = collections.Counter(B["func_short"][F][0] for F in ft if B["func_short"][F][3])
    w("  functions HIDDEN inside a rejected container or rejected whole as an out-of-line"
      " method (bx never looked at their bodies; their blocker sets are pure lower bounds): %d"
      % sum(hid.values()))
    w("  function CONCENTRATION (programs with the most function definitions; hidden in parens):")
    for P, c in conc.most_common(6):
        w("    %5d (%5d hidden)  %s" % (c, hid[P], P))
    w("  programs fully in subset (check whether they are trivial / preprocessed out):")
    for P, s in sorted(pt.items()):
        if not s:
            w("    %s  [%d function definitions]" % (P, conc[P]))

    ev = B["tag_events"]
    rows = table_counts(pt, ft, ev)
    tot_ev = sum(ev.values())
    pairs_p = sum(len(s) for s in pt.values())
    pairs_f = sum(len(s) for s in ft.values())
    w("\n(b) PER TAG (%d tags). Columns: PROGRAMS blocked | programs whose ONLY tag it is |"
      " FUNCTIONS blocked | functions whose only tag it is | RAW EVENTS (least trustworthy:"
      " counts sites, not programs)" % len(rows))
    w("%6s %5s %6s %5s %7s  %s" % ("progs", "soleP", "funcs", "soleF", "events", "tag"))
    for k, a, b, c, d, e in rows:
        w("%6d %5d %6d %5d %7d  %s" % (a, b, c, d, e, mark(k)))

    w("\n(c1) GREEDY SET COVER over PROGRAMS (tag granularity) -- THE RANKING COLUMN."
      " 'marginal' = programs newly fully-in-subset if this tag were fully supported,"
      " given every tag above it. UPPER BOUND. Baseline clean: %d/%d" % (nclean, nprog))
    w("%4s %8s %7s %6s  %s" % ("step", "marginal", "fracW", "cum", "tag"))
    for i, r in enumerate(greedy_cover(pt, args.cover_steps), 1):
        w("%4d %8d %7.2f %6d  %s" % (i, r["marginal"], r["frac_weight"], r["cumulative"], mark(r["key"])))
    fs = B["func_short"]
    pof = lambda F: fs[F][0]  # noqa: E731
    observed = {F: s for F, s in ft.items() if not fs[F][3]}
    observed_sub = {F: s for F, s in B["func_subs"].items() if not fs[F][3]}

    def fcover(title, sets):
        base = sum(1 for x in sets.values() if not x)
        w("\n%s. Baseline clean: %d/%d. 'progs' = distinct PROGRAMS the newly-clean functions"
          " come from (a big marginal from 1-3 programs is one file's fan-out, not breadth)"
          % (title, base, len(sets)))
        w("%4s %8s %5s %7s %6s  %s" % ("step", "marginal", "progs", "fracW", "cum", "key"))
        for i, r in enumerate(greedy_cover(sets, args.cover_steps, pof), 1):
            w("%4d %8d %5d %7.2f %6d  %s" % (i, r["marginal"], r["programs"], r["frac_weight"],
                                             r["cumulative"], mark(r["key"])))

    fcover("(c2) GREEDY SET COVER over ALL FUNCTIONS (tag granularity)", ft)
    fcover("(c3) GREEDY SET COVER over OBSERVED FUNCTIONS only (tag granularity) -- functions NOT"
           " inside a rejected class/namespace/template, i.e. bodies emitrust-bx actually"
           " inspected. Hidden functions' sets are just the container tag, so THIS is the honest"
           " function ranking", observed)
    fcover("(c3b) GREEDY SET COVER over OBSERVED FUNCTIONS (fine granularity: tag [sub-tag])",
           observed_sub)
    w("\n(c4) GREEDY SET COVER over PROGRAMS (fine granularity)")
    w("%4s %8s %7s %6s  %s" % ("step", "marginal", "fracW", "cum", "tag [sub]"))
    for i, r in enumerate(greedy_cover(B["prog_subs"], args.cover_steps), 1):
        w("%4d %8d %7.2f %6d  %s" % (i, r["marginal"], r["frac_weight"], r["cumulative"], mark(r["key"])))

    w("\n(d) FAN-OUT: raw events %d / distinct (program, tag) pairs %d = %.1fx ;"
      " / distinct (function, tag) pairs %d = %.1fx (container rejections are shared by"
      " every member, so the function ratio can be < 1)"
      % (tot_ev, pairs_p, tot_ev / max(1, pairs_p), pairs_f, tot_ev / max(1, pairs_f)))
    ot = B["other_events"]
    w("\n(e) `other` BUCKET (reasons no mapping rule matched): %d raw events, %d distinct reasons,"
      " %d programs touched" % (sum(ot.values()), len(ot),
                                 sum(1 for s in pt.values() if any(t.startswith("other: ") for t in s))))
    for reason, c in ot.most_common(40):
        w("  %5d  %s" % (c, reason))
    w("\nTop sub-tags by PROGRAMS (fine granularity):")
    sp = B["prog_subs"]
    sc = collections.Counter(k for s in sp.values() for k in s)
    for k, c in sc.most_common(40):
        w("  %5d  %s" % (c, k))
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bx", help="path to emitrust-bx.py (use a frozen snapshot); required unless --from-json")
    ap.add_argument("--clang", default="clang++")
    ap.add_argument("--out", required=True, help="output directory (census.json, summary.txt)")
    ap.add_argument("-j", type=int, default=4, help="workers (each clang JSON dump can be ~170MB; max 4 advised)")
    ap.add_argument("--timeout", type=int, default=300, help="per-file, per-stage seconds")
    ap.add_argument("--limit", type=int, default=0, help="only the first N corpus files (smoke test)")
    ap.add_argument("--files", nargs="*", help="explicit files instead of the corpus")
    ap.add_argument("--cover-steps", type=int, default=40)
    ap.add_argument("--from-json", help="re-render the summary from an existing census.json")
    args = ap.parse_args(argv)
    if args.from_json:
        if not args.bx:
            with open(args.from_json) as fh:
                args.bx = json.load(fh).get("bx", "?")
    elif not args.bx:
        ap.error("--bx is required for a sweep")
    else:
        args.bx = os.path.abspath(args.bx)
    os.makedirs(args.out, exist_ok=True)

    if args.from_json:
        with open(args.from_json) as fh:
            data = json.load(fh)
        records, corpus, notes = data["records"], [tuple(x) for x in data["corpus"]], data["notes"]
        for r in records:  # re-classify so the RULES table can be refined without a re-sweep
            for d in r.get("diags", []):
                d["tag"], d["sub"], d["matched"] = classify(d["reason"])
    else:
        if args.files:
            corpus = [(os.path.realpath(f), ["explicit"]) for f in args.files]
            notes = []
        else:
            corpus, notes = collect_corpus(REPO)
        if args.limit:
            corpus = corpus[:args.limit]
        jobs = [(p, args.bx, args.clang, args.timeout) for p, _ in corpus]
        records = []
        t0 = time.time()
        with multiprocessing.Pool(args.j, maxtasksperchild=1) as pool:
            for i, rec in enumerate(pool.imap_unordered(analyze, jobs), 1):
                records.append(rec)
                sys.stderr.write("[%d/%d %.0fs] %s %s\n" % (i, len(jobs), time.time() - t0,
                                                           rec["status"], os.path.relpath(rec["path"], REPO)))
        records.sort(key=lambda r: r["path"])
        with open(os.path.join(args.out, "census.json"), "w") as fh:
            json.dump({"bx": args.bx, "caveat": LOWER_BOUND_BANNER, "corpus": corpus, "notes": notes,
                       "records": records}, fh, indent=1)
    B = build(records, corpus, args)
    text = render(records, corpus, notes, B, args)
    with open(os.path.join(args.out, "summary.txt"), "w") as fh:
        fh.write(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
