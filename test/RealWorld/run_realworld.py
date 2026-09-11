#!/usr/bin/env python3
"""Real-world C / C++ corpus runner for emitrust-cc (Track 4, W4.0; FR-46).

Drives every program under a corpus directory through
``emitrust-cc --emit=crate --build`` and classifies the outcome:

  * REJECTED   -- the transpiler emitted a located diagnostic (rc != 0).
                  Tagged by BLOCKER category (see ``classify_blocker``); this
                  is the DEMAND SIGNAL the corpus exists to generate.
  * TRANSPILED -- the crate built and its stdout matched a clang-compiled
                  native build of the same sources (the differential oracle).
  * LIB_BUILT  -- the project has no ``main``, so emitrust-cc emitted a
                  LIBRARY crate (FR-51), and that crate compiled. See below
                  for why this is deliberately NOT ``TRANSPILED``.
  * MISCOMPILE -- the crate built but crashed, exited non-zero, or produced
                  stdout that differs from the native build; also a build that
                  reports success with no binary. Always fails, quarantine
                  aside -- a successfully transpiled program must be correct.

``LIB_BUILT`` IS STRICTLY WEAKER EVIDENCE THAN ``TRANSPILED``, and the two
must never be conflated. ``TRANSPILED`` means the transpiled program was
RUN and BEHAVED IDENTICALLY to the native build -- a differential fact about
semantics. A library crate has no entry point, so there is nothing to run and
no native oracle to run it against (``clang++`` cannot even link the sources
into an executable). All ``LIB_BUILT`` asserts is that the whole project was
translated and that ``rustc`` accepted the result: real evidence, since a
mistranslation usually fails to type-check, but evidence about
WELL-FORMEDNESS, not about behavior. A ``LIB_BUILT`` project could compute
entirely wrong answers and still score ``LIB_BUILT``.

It is ranked between ``REJECTED`` and ``TRANSPILED`` by the ratchet: losing
it is a regression, reaching it from ``REJECTED`` is an improvement that
must be ratcheted in, and reaching ``TRANSPILED`` from it is a further
improvement. Weakening a ``TRANSPILED`` project to ``LIB_BUILT`` is a
REGRESSION, not a lateral move -- that would mean a program that used to be
differentially validated no longer is.

Modeled on ``test/CTestSuite/run_c_testsuite.py`` (shared helpers copied so
this harness stays self-contained): per-crate ``target`` directories (a shared
CARGO_TARGET_DIR was measured and rejected there -- cargo's target-dir flock
serializes the pool), the two-way ratchet against a manifest of programs
EXPECTED to transpile, and an optional quarantine list.

Two corpus kinds share this one harness (``--corpus-kind``):

``c`` (the default, ``test/RealWorld/Inputs``)
    A program is either a single top-level ``<name>.c`` (one TU) or an
    immediate subdirectory ``<name>/`` whose ``*.c`` files compile together
    (multi-TU). The native oracle is ``clang -std=c11``. The manifest
    (``--manifest-format names``) is the flat set of program names EXPECTED
    to transpile. NOTE: ``argv`` VALUES are dropped at import (emitrust-cc's
    main wrapper passes only ``argc``), so command-line-argument programs are
    expected to reject.

``cpp`` (FR-46, ``test/RealWorld/Cpp/Inputs``)
    A program is an immediate subdirectory holding a ``compile_commands.json``
    that lists its ``.cpp`` translation units and their include directories.
    Paths inside that file are RELATIVE (so the corpus is checked in
    machine-independent) and are made absolute here, against the project
    directory. The native oracle is ``clang++ -std=c++17``. Because the C++
    input subset is young, most of this corpus is EXPECTED to reject, so the
    manifest records the OUTCOME of every project (``--manifest-format
    outcomes``) rather than only the transpiled set: that keeps the same
    two-way ratchet (a TRANSPILED project that stops transpiling is a
    regression; a REJECTED project that starts transpiling demands a
    ``--update``) while also making the checked-in manifest a readable,
    ranked backlog of C++ blockers.

Scoring in ``cpp`` mode is by OUTCOME **and**, since FR-44, by ITEM: every
project is additionally transpiled with ``emitrust-cc --emit=crate
--incremental --build``, which recovers from the items outside the subset,
proves the partial crate still BUILDS, and writes an ``emitrust-progress.json``
whose denominator is the FR-40 project item graph. That per-item score is
ratcheted against a per-project ``expected-items.txt`` checked in next to the
project's sources, with the same two-way discipline as the outcome manifest: a
``ported`` item that stops porting is a regression, and an item that starts
porting fails until it is ratcheted forward with ``--update``.
"""

import argparse
import difflib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
from collections import namedtuple
from concurrent.futures import ThreadPoolExecutor

TRANSPILE_BUILD_TIMEOUT = 120
NATIVE_BUILD_TIMEOUT = 60
RUN_TIMEOUT = 10

REJECTED = "REJECTED"
TRANSPILED = "TRANSPILED"
LIB_BUILT = "LIB_BUILT"
MISCOMPILE = "MISCOMPILE"

#: Every outcome an outcome-format manifest may name.
OUTCOMES = (REJECTED, TRANSPILED, LIB_BUILT, MISCOMPILE)

#: The outcomes that mean "emitrust-cc produced a crate that compiles",
#: ordered weakest-evidence-first. Used by the ratchet to decide which
#: direction a change moved in; see the module docstring for why LIB_BUILT is
#: strictly weaker than TRANSPILED and must not be conflated with it.
BUILT_OUTCOMES = (LIB_BUILT, TRANSPILED)

#: One corpus program: a display name, its translation units, and the include
#: directories its compilation needs (always empty for the C corpus, which has
#: no out-of-directory headers).
Program = namedtuple("Program", "name sources include_dirs")

#: One measured outcome. ``items`` is the FR-44 per-item payload (an
#: ``ItemScores``), or ``None`` when per-item scoring was not run or failed.
Result = namedtuple("Result", "name status tag detail items")

#: The FR-44 per-item payload for one project: ``report`` is the parsed
#: ``emitrust-progress.json`` (schema ``emitrust-progress/1``; see
#: tools/emitrust-cc/ProgressReport.h) and ``built`` says whether the partial
#: crate that report describes actually compiled with ``cargo build --release
#: --offline``. Both are needed: a per-item score over a crate that does not
#: build would be a fiction.
ItemScores = namedtuple("ItemScores", "report built")

#: The per-project per-item ledger, checked in beside the project's sources.
ITEMS_MANIFEST_NAME = "expected-items.txt"


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitrust-cc", required=True, dest="emitrust_cc",
                        help="Path to (or bare name of) the emitrust-cc binary.")
    parser.add_argument("--clang", default="clang",
                        help="Native differential-oracle compiler. Defaults to"
                             " 'clang'; pass 'clang++' with --corpus-kind cpp.")
    parser.add_argument("--corpus-kind", choices=("c", "cpp"), default="c",
                        dest="corpus_kind",
                        help="Corpus layout and language: 'c' (top-level *.c"
                             " plus multi-TU subdirs, clang -std=c11 oracle) or"
                             " 'cpp' (compile_commands.json-described project"
                             " subdirs, clang++ -std=c++17 oracle).")
    parser.add_argument("--corpus", required=True,
                        help="Directory of corpus programs.")
    parser.add_argument("--manifest", required=True,
                        help="Expectation manifest; see --manifest-format.")
    parser.add_argument("--manifest-format", choices=("names", "outcomes"),
                        default="names", dest="manifest_format",
                        help="'names': a flat list of program names EXPECTED to"
                             " transpile (the C corpus). 'outcomes': one"
                             " '<name> <OUTCOME> [tag]' line per program (the"
                             " C++ corpus, most of which is expected to"
                             " reject).")
    parser.add_argument("--workdir", required=True,
                        help="Scratch directory for per-program crate output.")
    parser.add_argument("--known-miscompiles", default=None,
                        help="Optional quarantine list of known-miscompiling program names.")
    parser.add_argument("--update", action="store_true",
                        help="Rewrite the manifest with the current measured"
                             " outcomes instead of failing on drift.")
    return parser.parse_args(argv)


def resolve_tool(value):
    """Resolve a tool argument to an absolute executable, or fall back to PATH."""
    if os.sep in value:
        candidate = os.path.abspath(value)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
        raise SystemExit("error: tool not executable: %s" % candidate)
    found = shutil.which(value)
    if found is None:
        raise SystemExit("error: cannot find %r on PATH" % value)
    return found


def sanitize_crate_binary_name(stem):
    """Mirror emitrust-cc's crate-name sanitization (CrateEmitter sanitizeCrateName)."""
    lowered = stem.lower()
    sanitized = "".join(
        ch if (ch.isascii() and (ch.isdigit() or "a" <= ch <= "z" or ch == "_")) else "_"
        for ch in lowered
    )
    if not sanitized:
        return "transpiled"
    if sanitized[0].isdigit():
        return "_" + sanitized
    return sanitized


def load_name_list(path, required):
    names = set()
    if not os.path.isfile(path):
        if required:
            raise SystemExit("error: manifest not found: %s" % path)
        return names
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.split("#", 1)[0].strip()
            if stripped:
                names.add(stripped)
    return names


def run_command(cmd, timeout, cwd=None, env=None, argv0=None):
    """Run ``cmd`` with a hard timeout; SIGKILL the whole group on timeout.

    ``argv0`` overrides the child's ``argv[0]`` (its ``sys.argv[0]``/C
    ``argv[0]``) while still executing the file at ``cmd[0]``. C99-43 C3: the
    native oracle and the crate binary live at DIFFERENT absolute paths, so a
    program that echoes ``argv[0]`` (now importable via the admitted argv
    table) would diverge spuriously on the path alone. Passing the SAME
    ``argv0`` to both runs equalizes it; every existing program ignores
    ``argv[0]``, so this is a no-op for them.
    """
    full_env = None
    if env:
        full_env = dict(os.environ)
        full_env.update(env)
    if argv0 is not None:
        proc = subprocess.Popen(
            [argv0] + list(cmd[1:]), executable=cmd[0],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, cwd=cwd, env=full_env,
            start_new_session=True,
        )
    else:
        proc = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, cwd=cwd, env=full_env,
            start_new_session=True,
        )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        return proc.returncode, stdout, stderr, False
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        stdout, stderr = proc.communicate()
        return proc.returncode, stdout, stderr, True


def first_line(data):
    for line in data.decode("utf-8", errors="replace").splitlines():
        if line.strip():
            return line.strip()
    return ""


#: ClangTool's per-file progress chatter, printed on stderr ahead of any
#: diagnostic whenever emitrust-cc is handed more than one translation unit.
_TOOL_PROGRESS_RE = re.compile(r"^\[\d+/\d+\] Processing file ")


def first_diagnostic_line(data):
    """The first line of ``data`` that is actually a diagnostic.

    ``first_line`` alone is wrong for a MULTI-TU compile: ClangTool prints a
    "[k/n] Processing file ..." line per input before any diagnostic reaches
    stderr, so the naive first line is progress chatter and every multi-TU
    rejection would tag as "other". Progress lines are dropped, then the first
    line carrying "error:" wins; a compiler CRASH (which prints a stack dump
    and no "error:" line) falls back to the first remaining line, which is
    what ``classify_blocker``'s crash heuristic reads.
    """
    lines = [line.strip()
             for line in data.decode("utf-8", errors="replace").splitlines()
             if line.strip() and not _TOOL_PROGRESS_RE.match(line.strip())]
    for line in lines:
        if "error:" in line:
            return line
    return lines[0] if lines else ""


def output_diff_snippet(expected, actual, limit=12):
    diff = difflib.unified_diff(
        expected.decode("utf-8", errors="replace").splitlines(),
        actual.decode("utf-8", errors="replace").splitlines(),
        fromfile="native", tofile="crate", lineterm="",
    )
    lines = list(diff)[:limit]
    if not lines:
        return "(outputs differ only in trailing bytes/newlines)"
    return "\n".join("    " + line for line in lines)


# Blocker-tag heuristic over the first diagnostic line. Ordered; first match
# wins. free/realloc/malloc/calloc reject via the generic system-header path,
# so the symbol name is parsed out to separate dynamic-memory from other libc.
# The generic "pointer assigned a non-address value" / "no known target object"
# wordings are shared by local-malloc AND strchr-result binds, so those are
# refined by reading the cited source line (the diagnostic carries file:line).
_SYS_HEADER_RE = re.compile(r"call to '([^']+)' declared in a system header")
_LOC_RE = re.compile(r"^(.+?):(\d+):\d+: error:")
_DYNMEM_NAMES = {"malloc", "calloc", "realloc", "free", "aligned_alloc"}
# FR-230: `alloca` joins the list, hand-mirroring `citedLineAllocates`
# in lib/ImportC/RejectionLedger.cpp. Without it a stack allocation tags
# as `pointer-local-nonaddress` and the census reads it as the same lever
# as an unrelated pointer-member load.
_ALLOC_KEYWORDS = ("malloc", "calloc", "realloc", "aligned_alloc", "alloca")
_BLOCKER_SUBSTRINGS = [
    ("use of main's argv", "argv"),
    # C99-43 slice 1: the cursor-parameter rejections split out of the
    # generic ptr-to-ptr bucket, listed ABOVE it so first-match-wins
    # routes them (every wording also contains "pointer-to-pointer" or
    # "cursor parameter").
    ("escapes the cursor-parameter shape", "ptr-to-ptr-shape-escape"),
    ("write through a cursor parameter", "ptr-to-ptr-shape-escape"),
    ("written with a null pointer", "ptr-to-ptr-null-write"),
    # C99-43 C1 narrowed this family: single-global-or-NULL writes are
    # admitted (the Option-cell mapping), so the needle widened from
    # "written with a global address" to catch the residual wordings --
    # "more than one global address" and "a global address outside the
    # single-global-or-NULL shape" -- which stay the FR-62 front.
    ("global address", "ptr-to-ptr-global-target"),
    ("write sites disagree on the source region", "ptr-to-ptr-shape-escape"),
    ("write must execute unconditionally", "ptr-to-ptr-shape-escape"),
    ("does not root in a sibling slice parameter", "ptr-to-ptr-shape-escape"),
    ("pointer-to-pointer", "ptr-to-ptr"),
    # Mirrors the RejectionLedger.cpp row of the same position: a negative
    # element index below a Phase-1b slice parameter's origin.
    (
        "negative element index through a slice parameter",
        "slice-param-negative-index",
    ),
    # FR-229: the object-representation BYTE VIEW family -- a padded
    # aggregate (no determinate image), a global base (its image would come
    # from a staged copy), and a mutable view with no post-call write-back
    # point (the callee's writes would be silently lost). Kept as three
    # tags because they are three separate pieces of work, and placed above
    # the generic wordings so first-match-wins routes them. None overlaps
    # the older CTS-BR "byte view of an aggregate with non-byte members".
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("byte view of an aggregate with interior padding", "byte-view-padding"),
    ("byte view of the global object", "byte-view-global"),
    ("byte view with no write-back point", "byte-view-writeback"),
    # FR-234 rung 1: the strto* family's endptr frontier. With a NULL
    # endptr strtol/strtoul/strtod now import, so a still-refused call is
    # specifically the out-parameter shape (rung 2) and must keep a tag of
    # its own rather than falling into `other` once the name has left the
    # `libc:<name>` bucket. Mirrors lib/ImportC/RejectionLedger.cpp.
    ("with a non-null endptr argument", "strtox-endptr"),
    ("returned pointer value", "returned-pointer"),
    ("pointer return type", "returned-pointer"),
    ("return sites disagree", "returned-pointer"),
    ("global pointer bound to a string literal", "global-string-cursor"),
    ("unsupported: allocation", "dynamic-memory"),
    ("pointer struct member of an externally", "pointer-member-cross-tu"),
    ("static-binding model", "self-ref-pointer-member"),
    ("variadic function", "variadic-cross-tu"),
    # FR-77: a fn-ptr constant naming a function no TU defines is refused at
    # the address-taking site, so the containing item carries the blocker.
    ("address of undefined function", "fnptr-undefined-target"),
    # FR-103: an extern global no TU defines costs each referencing item
    # (stubbed at finalize); also catches the single-TU "without a
    # definition" spelling. Mirrors lib/ImportC/RejectionLedger.cpp.
    ("extern global variable", "undefined-extern-global"),
    # FR-129 half (a): glibc's <ctype.h> classifiers are macros over a
    # locale table reached through __ctype_b_loc(); previously [other].
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("locale ctype table lookup", "ctype-table"),
    # FR-129 half (b): a translation unit that installs a locale keeps the
    # classifier rejection, under its own tag.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("classifier in a translation unit that calls", "ctype-locale"),
    # FR-113 admitted scoped enums; the residual enum-definition gates
    # (keyword names/enumerators, values outside i32, empty, cross-TU shape
    # conflict) previously tabulated [other].
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("unsupported: enum name", "enum-def-rejected"),
    ("unsupported: enumerator", "enum-def-rejected"),
    ("enum with no enumerators", "enum-def-rejected"),
    ("conflicting definition of enum", "enum-def-rejected"),
    # FR-108 enum arm: the same-TU half of the enum dedup guard (two enums
    # of ONE unit composing one emitted symbol). Language-agnostic, like
    # record-name-clash. Mirrors lib/ImportC/RejectionLedger.cpp.
    ("collides with the emitted name of a different enum", "enum-name-clash"),
    # FR-122: cross-TU record conflicts (field shape OR, now, a divergent
    # member surface via the ODR-hashed key) get their own tag instead of
    # [other]. Fires in plain C too. Mirrors lib/ImportC/RejectionLedger.cpp.
    ("conflicting definition of struct", "struct-shape-conflict"),
    # FR-50: not a blocker of its own -- the item names a type some OTHER
    # item's rejection removed. FR-49's root-blocker table credits the real
    # cause; this tag only keeps the cascade out of the "other" bucket.
    ("was rejected, so a type naming it", "rejected-type-cascade"),
]
# Wordings shared across blockers; refined by the cited source line's content.
_AMBIGUOUS_POINTER = ("pointer assigned a non-address value", "with no known target object")

# C++-input blocker tags (FR-46). Every wording below is emitted only from a
# C++-only code path in lib/ImportC (a CXXRecordDecl walk, mapType's reference
# case, or the STL recognition table), so a C program can never match any of
# them -- the C corpus's tabulation is unchanged by this table's existence.
_CXX_BLOCKER_SUBSTRINGS = [
    ("base classes are not supported", "cxx-inheritance"),
    # W2.18 admitted the SINGLE public non-virtual base as an ordinary first
    # field; the wording above is retained for the residual shapes (multiple,
    # virtual and non-public inheritance, an undefined or template-
    # specialization base). The wordings below are the measured miscompile
    # channels around the admitted subset, each with its own tag.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    # W2.26 retired "base class with a destructor"/cxx-drop-base and
    # "virtual destructor"/cxx-virtual-destructor: both shapes are admitted
    # (transitive drop predicate) and each wording had exactly one emitter.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("inherited member of an empty base class", "cxx-inheritance-empty-base"),
    ("constructor of an empty base class", "cxx-inheritance-empty-base"),
    ("inherited member through a pointer to a derived class", "cxx-inheritance-upcast"),
    # FR-120 admitted method calls/upcast bindings through LOCAL struct
    # pointers; a mutating method through a pointer to a GLOBAL object has
    # no writeback flush and keeps its own wording.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("mutating method call through a pointer to a global object", "cxx-method-global-receiver"),
    ("inherited access through a non-struct place", "cxx-inheritance"),
    ("base constructor initializer", "cxx-inheritance"),
    # W2.17 admitted the non-virtual, same-TU-defined destructor; the generic
    # wording is retained for the residual (union) shape, and every measured
    # miscompile channel around the admitted subset gets its own tag.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("destructor with no definition in this translation unit", "cxx-destructor-no-body"),
    ("destructor collides with the member function 'dtor'", "cxx-destructor-name-clash"),
    # FR-185: the constructor twin, live since the constructor's fixed base
    # name became `ctor` (a legal C++ member spelling; `new` was a keyword).
    ("constructor collides with the member function ", "cxx-constructor-name-clash"),
    ("struct member of a class with a destructor", "cxx-drop-member"),
    ("array of a class with a destructor", "cxx-drop-array"),
    ("global or static object of a class with a destructor", "cxx-drop-global"),
    ("class with a destructor passed or returned by value", "cxx-drop-by-value"),
    ("value copy of a class with a destructor", "cxx-drop-by-value"),
    ("outside a function, loop, or branch body", "cxx-drop-scope"),
    ("in a loop whose increment has side effects", "cxx-drop-scope"),
    ("in a loop whose exit path has side effects", "cxx-drop-scope"),
    ("do-while loop whose condition has side effects", "cxx-drop-scope"),
    ("user-declared destructor", "cxx-destructor"),
    # W2.19a retired "unsupported: virtual method"/cxx-virtual: the
    # class-level gate is gone (virtual methods on values statically
    # bind); the dynamic residual is the call-site pointer fence.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("virtual method call through a pointer", "cxx-virtual-call"),
    ("virtual or unresolved member call", "cxx-virtual-call"),
    # FR-112 containment: an omitted member's USE sites. The first needle
    # must sit ABOVE the "overloaded operator" row (its message contains
    # both substrings; first match wins). Mirrors
    # lib/ImportC/RejectionLedger.cpp.
    ("omitted from class", "cxx-omitted-member"),
    ("call to unimported method", "cxx-omitted-member"),
    ("overloaded operator", "cxx-operator-overload"),
    # FR-114: residual overload-set collisions the widened suffix table
    # cannot split (frozen member `i` pairs, long/long long, two fn-pointer
    # params, separator-free member code aliasing). Previously hid inside
    # the cross-TU "conflicting definition" wording and tabulated [other].
    # No substring overlap with the "overloaded operator" row above.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("C++ overload set for", "cxx-overload-collision"),
    # FR-117: a user-defined conversion function. The member is OMITTED from
    # the class, but its residual use positions (the explicit
    # `c.operator int()` spelling, an out-of-line definition) are located
    # rejections; the implicit and static_cast uses arrive as
    # CK_UserDefinedConversion. Both were untagged (catch-all `other`) before
    # FR-117 shifted mass into them. Mirrors lib/ImportC/RejectionLedger.cpp.
    ("unsupported: conversion function", "cxx-conversion-function"),
    ("unsupported cast (UserDefinedConversion)", "cxx-user-conversion"),
    # FR-118: the class-level gate whose late position was the half-import
    # miscompile; also previously untagged.
    # Mirrors lib/ImportC/RejectionLedger.cpp.
    ("copy/move/delegating constructor", "cxx-copy-ctor"),
    # W2.23 admitted the user-provided `T(const T&)` copy constructor; the
    # broad needle catches the wave's residual wordings in one row (the
    # non-const-`T&` shape, the NRVO-candidate return, the E0277 array
    # boundary), and the honest implicit-copy-assignment wordings get their
    # own tag (copy-ASSIGNMENT is a different AST node and a different
    # backlog item). Mirrors lib/ImportC/RejectionLedger.cpp.
    ("copy constructor", "cxx-copy-ctor"),
    ("implicit copy assignment", "cxx-copy-assign"),
    # FR-48 landed reference PARAMETERS; the four wordings below are the
    # residual reference positions, all still tagged `cxx-references` so the
    # backlog keeps ranking them as one blocker. "rvalue reference types are
    # not yet supported" needs no entry of its own -- it ends with the
    # generic wording and matches it as a substring.
    ("reference return types are not yet supported", "cxx-references"),
    ("reference struct members are not yet supported", "cxx-references"),
    ("reference-to-pointer parameter", "cxx-references"),
    ("reference-to-array parameter", "cxx-references"),
    ("reference types are not yet supported", "cxx-references"),
    # W2.22 ostream frontier; mirrors lib/ImportC/RejectionLedger.cpp. The
    # generic ("std::ostream << operand") row is LAST of the six so the
    # specific shapes above it win first-match.
    ("result of a std::ostream << chain must be unused", "cxx-ostream-value-use"),
    ("pointer std::ostream << operand prints a nondeterministic address", "cxx-ostream-pointer-operand"),
    ("std::ostream << operand must be a string literal", "cxx-ostream-cstr-operand"),
    ("is not a recognized std::ostream manipulator", "cxx-ostream-manipulator"),
    ("std::ostream << string literal", "cxx-ostream-string-literal"),
    ("std::ostream << operand", "cxx-ostream-operand-type"),
    # W2.20 std::map / std::set frontier; mirrors
    # lib/ImportC/RejectionLedger.cpp. Every row sits BEFORE the three
    # generic STL rows below so first-match keeps them distinguishable.
    # The first two are PERMANENT rejections, not backlog items: an
    # unordered container's iteration order is unspecified and a multi-
    # container holds duplicate keys, so no Rust container reproduces
    # either byte for byte.
    ("iteration order is unspecified", "stl-unordered-container"),
    ("stores duplicate keys", "stl-multi-container"),
    ("with a comparator other than std::less", "stl-map-comparator"),
    ("is not in the supported ordered key set", "stl-map-key-type"),
    ("is not in the supported value set", "stl-map-value-type"),
    ("does not overwrite an existing key", "stl-map-insert"),
    ("iterators are only recognized in the find(k) != end() idiom", "stl-map-iterator"),
    ("is a read-only place", "stl-map-at-write"),
    ("requires a structured binding", "stl-map-ranged-for"),
    # W2.21 std::unique_ptr frontier; mirrors
    # lib/ImportC/RejectionLedger.cpp. Every row sits BEFORE the three
    # generic STL rows below so first-match keeps them distinguishable.
    # stl-shared-ptr is a PERMANENT rejection (Rc/Arc have different
    # aliasing and the subset has no shared-ownership model);
    # stl-unique-ptr-nullable is THE wave boundary (a bare Box<T> cannot be
    # null), and its count is the ranking signal for a later
    # Option<Box<T>> wave.
    ("the std::unique_ptr<T[]> array form", "stl-unique-ptr-array-form"),
    ("with a deleter other than std::default_delete", "stl-unique-ptr-deleter"),
    ("is not in the supported payload set", "stl-unique-ptr-payload-type"),
    ("std::unique_ptr payload does not match", "stl-unique-ptr-payload-type"),
    ("no model for shared ownership", "stl-shared-ptr"),
    ("a Box<T> cannot be null", "stl-unique-ptr-nullable"),
    ("moved-from std::unique_ptr", "stl-unique-ptr-move"),
    ("hands out a raw pointer to the payload", "stl-unique-ptr-raw-pointer"),
    ("std::make_unique is only recognized as the initializer", "stl-make-unique-position"),
    ("std::make_unique argument type does not match", "stl-make-unique-argument"),
    ("this std::unique_ptr initializer shape", "stl-unique-ptr-construct"),
    ("std::unique_ptr shape could not be determined", "stl-unique-ptr-construct"),
    ("std::make_unique of a class with in-class member initializers", "stl-unique-ptr-construct"),
    ("std::make_unique constructor", "stl-unique-ptr-construct"),
    ("called through a std::unique_ptr", "stl-unique-ptr-ref-argument"),
    ("called through std::make_unique", "stl-unique-ptr-ref-argument"),
    # FR-188 shares the tag with the two rows above: all three name the same
    # future work (the payload borrow following the callee's parameter
    # mutability). Mirrors lib/ImportC/RejectionLedger.cpp.
    ("mutable reference argument borrowed from a std::unique_ptr",
     "stl-unique-ptr-ref-argument"),
    ("is not a recognized STL type", "stl-unrecognized-type"),
    ("is not a recognized STL method", "stl-unrecognized-method"),
    ("receiver is not a recognized STL", "stl-unrecognized-receiver"),
    # W2.16 class-template frontier; mirrors lib/ImportC/RejectionLedger.cpp.
    ("explicit class template specialization", "cxx-class-template-explicit-spec"),
    ("partial class template specialization", "cxx-class-template-partial-spec"),
    ("non-type template argument in class template instantiation", "cxx-class-template-nttp"),
    ("variadic class template (template parameter pack)", "cxx-class-template-pack"),
    ("class template instantiation collides with the existing struct", "cxx-class-template-name-clash"),
    # FR-108 record-name clash (language-agnostic; mirrors
    # lib/ImportC/RejectionLedger.cpp).
    ("collides with the emitted name of a different struct", "record-name-clash"),
    # W2.24 exceptions-as-Result-threading frontier. The uncaught row sits
    # FIRST: the call-outside-try wording contains both "potentially-
    # throwing function" and "cannot propagate exceptions", and first-match
    # must file it under the uncaught tag. Mirrors
    # lib/ImportC/RejectionLedger.cpp.
    ("cannot propagate exceptions", "cxx-exception-uncaught"),
    ("thrown exception payload", "cxx-exception-payload"),
    ("potentially-throwing function", "cxx-exception-closure"),
    ("a throw reaching a noexcept function", "cxx-exception-noexcept"),
    ("catch by reference", "cxx-exception-catch"),
    ("catch of a type other than", "cxx-exception-catch"),
    ("more than one catch handler", "cxx-exception-catch"),
    ("nested inside another try or catch", "cxx-exception-catch"),
    ("rethrow outside a catch handler", "cxx-exception-catch"),
    ("try/catch outside a translation-unit-level", "cxx-exception-catch"),
    ("declared inside a try statement", "cxx-exception-catch"),
    ("unsupported top-level declaration", "unsupported-top-level-decl"),
]
# Generic dispatch fallbacks that name the offending AST node class. These are
# language-agnostic (a C program can reach them too); refining them from the
# catch-all "other" into a node-named tag makes the tabulation a directly
# actionable backlog. Tags are display-only for the C corpus, whose manifest
# records program names, not tags.
_NODE_NAMED_RE = [
    (re.compile(r"unsupported assignable expression: (\w+)"), "unsupported-assign-expr:"),
    (re.compile(r"unsupported expression: (\w+)"), "unsupported-expr:"),
    (re.compile(r"unsupported statement: (\w+)"), "unsupported-stmt:"),
]


def _cited_source_line(diagnostic):
    """Read the source line the diagnostic points at (best effort), or ''."""
    match = _LOC_RE.match(diagnostic)
    if not match:
        return ""
    path, line_no = match.group(1), int(match.group(2))
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for index, text in enumerate(handle, start=1):
                if index == line_no:
                    return text
    except OSError:
        pass
    return ""


def classify_blocker(diagnostic):
    """Map a first-line diagnostic to a coarse blocker tag for the survey."""
    if "PLEASE submit a bug report" in diagnostic or "Stack dump" in diagnostic:
        return "crash"
    match = _SYS_HEADER_RE.search(diagnostic)
    if match:
        name = match.group(1)
        return "dynamic-memory" if name in _DYNMEM_NAMES else "libc:" + name
    for needle, tag in _BLOCKER_SUBSTRINGS:
        if needle in diagnostic:
            return tag
    for needle, tag in _CXX_BLOCKER_SUBSTRINGS:
        if needle in diagnostic:
            return tag
    for pattern, prefix in _NODE_NAMED_RE:
        match = pattern.search(diagnostic)
        if match:
            return prefix + match.group(1)
    if any(needle in diagnostic for needle in _AMBIGUOUS_POINTER):
        source = _cited_source_line(diagnostic)
        if any(kw in source for kw in _ALLOC_KEYWORDS):
            return "dynamic-memory"
        if "strchr" in source or "strrchr" in source:
            return "strchr-result-bind"
        return "pointer-local-nonaddress"
    return "other"


def discover_c_programs(corpus_dir):
    """Return sorted Programs: top-level *.c = single TU, each immediate subdir
    with *.c = one multi-TU program."""
    programs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if os.path.isfile(path) and entry.endswith(".c"):
            programs.append(Program(entry[: -len(".c")], [path], []))
        elif os.path.isdir(path):
            sources = sorted(
                os.path.join(path, f) for f in os.listdir(path) if f.endswith(".c")
            )
            if sources:
                programs.append(Program(entry, sources, []))
    return programs


def _entry_argv(entry):
    """The compile command of one compile_commands.json entry, as a list."""
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def _include_dirs_from_argv(argv, base):
    """Absolute include directories named by -I in ``argv`` (both the joined
    ``-Idir`` and the separated ``-I dir`` spellings), resolved against
    ``base``."""
    dirs = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        value = None
        if arg == "-I" and index + 1 < len(argv):
            value = argv[index + 1]
            index += 1
        elif arg.startswith("-I") and len(arg) > 2:
            value = arg[2:]
        if value is not None:
            dirs.append(os.path.normpath(os.path.join(base, value)))
        index += 1
    return dirs


def read_compile_commands(project_dir):
    """Parse ``project_dir/compile_commands.json`` into (sources, include_dirs).

    Every path in the checked-in file is RELATIVE (a machine-independent
    corpus); ``directory`` is resolved against ``project_dir`` and everything
    else against ``directory``, so absolute paths are produced here at run
    time and never stored in the repository. Absolute paths in the file, if a
    generator ever writes one, are honored as-is.
    """
    db_path = os.path.join(project_dir, "compile_commands.json")
    with open(db_path, "r", encoding="utf-8") as handle:
        entries = json.load(handle)

    sources = []
    include_dirs = []
    for entry in entries:
        directory = entry.get("directory", ".")
        if not os.path.isabs(directory):
            directory = os.path.join(project_dir, directory)
        directory = os.path.normpath(directory)

        source = entry["file"]
        if not os.path.isabs(source):
            source = os.path.join(directory, source)
        source = os.path.normpath(source)
        if not os.path.isfile(source):
            raise SystemExit("error: %s names a missing source: %s"
                             % (db_path, source))
        if source not in sources:
            sources.append(source)

        for include in _include_dirs_from_argv(_entry_argv(entry), directory):
            if include not in include_dirs:
                include_dirs.append(include)

    if not sources:
        raise SystemExit("error: %s lists no translation units" % db_path)
    return sorted(sources), include_dirs


def discover_cpp_programs(corpus_dir):
    """Return sorted Programs: each immediate subdirectory holding a
    compile_commands.json is one multi-TU C++ project."""
    programs = []
    for entry in sorted(os.listdir(corpus_dir)):
        path = os.path.join(corpus_dir, entry)
        if not os.path.isdir(path):
            continue
        if not os.path.isfile(os.path.join(path, "compile_commands.json")):
            continue
        sources, include_dirs = read_compile_commands(path)
        programs.append(Program(entry, sources, include_dirs))
    return programs


def discover_programs(corpus_dir, kind):
    """Dispatch discovery on the corpus kind."""
    if kind == "cpp":
        return discover_cpp_programs(corpus_dir)
    return discover_c_programs(corpus_dir)


# ---------------------------------------------------------------------------
# FR-44 -- per-item incremental scoring.
#
# The C++ corpus (FR-46) used to be graded by whole-project OUTCOME only: a
# project either transpiled end to end or it did not, and today most do not.
# That is a coarse signal -- it cannot distinguish "3 of 40 functions ported"
# from "0 of 40". FR-44's `emitrust-cc --emit=crate --incremental` supplies
# the finer one: it recovers from every item outside the subset (stubbing what
# it can, dropping the rest), emits a crate that still BUILDS, and writes
# `emitrust-progress.json` next to it -- one record per program item, with the
# denominator taken from the FR-40 project item graph so it counts what the
# project HAS rather than what the importer happened to reach.
#
# `collect_per_item_scores` below runs that invocation and parses the report;
# the payload rides in `Result.items` and is consumed in three places:
# `write_outcome_manifest` (an advisory `# ported k/n` comment per project),
# `ratchet_items` (the authoritative two-way per-item gate against each
# project's checked-in `expected-items.txt`), and the report loop.
# ---------------------------------------------------------------------------
def collect_per_item_scores(tool, program, workdir, enabled):
    """Return the FR-44 ``ItemScores`` for ``program``, or None.

    ``enabled`` is false for the C corpus, whose programs are scored by
    outcome alone; per-item scoring is a C++-subset instrument and running it
    there would only double every cargo build. A None return means "per-item
    scoring unavailable" to every consumer, and is also what a transpiler
    failure or an unparseable report produces -- the incremental mode is meant
    to survive a partially unsupported project, so its own failure is a real
    signal rather than something to paper over.
    """
    if not enabled:
        return None
    name, sources, include_dirs = program
    crate_dir = os.path.join(workdir, name + ".incremental")
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)
    include_flags = ["-I" + d for d in include_dirs]

    # --build is part of the measurement, not a nicety: FR-44's headline claim
    # is that a partially ported project still yields a COMPILING crate, so the
    # gate proves it on every corpus project on every run.
    rc, _, _, timed_out = run_command(
        [tool, "--emit=crate", "--incremental", *sources, *include_flags,
         "-o", crate_dir, "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    report_path = os.path.join(crate_dir, "emitrust-progress.json")
    try:
        with open(report_path, "r", encoding="utf-8") as handle:
            report = json.load(handle)
    except (OSError, ValueError):
        return None
    return ItemScores(report, built=(rc == 0 and not timed_out))


def ported_fraction(scores):
    """(ported, portable) item counts of an ``ItemScores``, or (0, 0)."""
    if scores is None:
        return 0, 0
    totals = scores.report.get("totals", {})
    return totals.get("ported", 0), totals.get("graph_items", 0)


def format_ported_fraction(scores):
    """'3/8 (37.5%)' for a report, or '-' when there is no per-item score."""
    if scores is None:
        return "-"
    if scores.report.get("denominator_source") != "item-graph":
        return "n/a (no item graph)"
    ported, portable = ported_fraction(scores)
    permille = scores.report.get("totals", {}).get("ported_permille", 0)
    return "%d/%d (%d.%d%%)" % (ported, portable, permille // 10, permille % 10)


def run_single_program(tool, native_cc, native_std, program, workdir,
                       score_items):
    """Transpile+build, then differentially validate against a native build."""
    name, sources, include_dirs = program
    crate_dir = os.path.join(workdir, name)
    if os.path.isdir(crate_dir):
        shutil.rmtree(crate_dir)
    include_flags = ["-I" + d for d in include_dirs]

    def result(status, tag, detail):
        return Result(name, status, tag, detail,
                      collect_per_item_scores(tool, program, workdir,
                                              score_items))

    rc, _, stderr, timed_out = run_command(
        [tool, "--emit=crate", *sources, *include_flags, "-o", crate_dir,
         "--build"],
        TRANSPILE_BUILD_TIMEOUT,
    )
    if timed_out:
        return result(REJECTED, "timeout",
                      "transpile/build timed out after %ss" % TRANSPILE_BUILD_TIMEOUT)
    if rc != 0:
        detail = first_diagnostic_line(stderr)
        return result(REJECTED, classify_blocker(detail), detail)

    # FR-51: a project with no ``main`` is emitted as a LIBRARY crate, whose
    # root is src/lib.rs and which produces no executable. Detect that from
    # the crate emitrust-cc actually wrote rather than by re-deriving the
    # rule, and stop here: there is no binary to run and -- just as decisive
    # -- no native oracle to run it against, since the native compiler cannot
    # link an executable out of sources that define no entry point either.
    # This check must precede BOTH the missing-binary MISCOMPILE below (a
    # library legitimately has no binary) and the native build (which would
    # fail to link and be misreported as a miscompile).
    if os.path.isfile(os.path.join(crate_dir, "src", "lib.rs")):
        return result(LIB_BUILT, "",
                      "library crate (no main); built but not run -- no"
                      " executable oracle exists for it")

    binary = os.path.join(crate_dir, "target", "release",
                          sanitize_crate_binary_name(name))
    if not os.path.isfile(binary):
        return result(MISCOMPILE, "",
                      "build reported success but binary missing: %s" % binary)

    # Native oracle: compile the same sources natively and diff stdout.
    native = os.path.join(crate_dir, "native_oracle")
    rc, _, nstderr, ntimed = run_command(
        [native_cc, native_std, "-w", *include_flags, *sources, "-o", native],
        NATIVE_BUILD_TIMEOUT)
    if ntimed or rc != 0:
        return result(MISCOMPILE, "",
                      "native build failed: " + first_line(nstderr))

    # C99-43 C3: equalize argv[0] across the two runs (see run_command) so a
    # program echoing the program name diffs on content, not on its path.
    run_argv0 = "./" + name
    n_rc, n_out, _, n_timed = run_command([os.path.abspath(native)], RUN_TIMEOUT, cwd=crate_dir, argv0=run_argv0)
    if n_timed or n_rc != 0:
        return result(MISCOMPILE, "", "native oracle run failed (rc=%s)" % n_rc)

    c_rc, c_out, c_err, c_timed = run_command([os.path.abspath(binary)], RUN_TIMEOUT, cwd=crate_dir, argv0=run_argv0)
    if c_timed:
        return result(MISCOMPILE, "", "crate binary timed out after %ss" % RUN_TIMEOUT)
    if c_rc != 0:
        detail = "crate binary exited %s (expected %s)" % (c_rc, n_rc)
        line = first_line(c_err)
        if line:
            detail += "; stderr: " + line
        return result(MISCOMPILE, "", detail)
    if c_out != n_out:
        return result(MISCOMPILE, "",
                      "stdout mismatch vs native:\n" + output_diff_snippet(n_out, c_out))
    return result(TRANSPILED, "", "")


def write_name_manifest(path, transpiled):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# RealWorld corpus: program names emitrust-cc transpiles AND whose\n"
            "# crate output matches a clang-native build (Track 4, W4.0).\n"
            "# One program name per line. Regenerated via: run_realworld.py ... --update\n"
        )
        for name in sorted(transpiled):
            handle.write(name + "\n")


def write_outcome_manifest(path, results):
    """Write the '<name> <OUTCOME> [tag]' manifest used by the C++ corpus."""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# RealWorld C++ corpus (FR-46): the MEASURED outcome of every\n"
            "# project, one per line, as '<name> <OUTCOME> [blocker-tag]'.\n"
            "#\n"
            "# This corpus exists to GENERATE DEMAND for the C++ input subset,\n"
            "# not to be a conformance target: a REJECTED line is the expected,\n"
            "# honest baseline and a standing backlog item, not a failure. The\n"
            "# ratchet is two-way -- a TRANSPILED project that stops\n"
            "# transpiling fails the gate as a regression, and a REJECTED\n"
            "# project that starts transpiling fails until it is ratcheted\n"
            "# forward. The blocker tag is advisory: a changed tag is reported\n"
            "# loudly but does not fail the gate, since it is a heuristic over\n"
            "# diagnostic wording.\n"
            "#\n"
            "# LIB_BUILT (FR-51) is a project with no 'main': emitrust-cc\n"
            "# emitted a LIBRARY crate and that crate compiled. It ranks\n"
            "# between REJECTED and TRANSPILED and is deliberately NOT the\n"
            "# same as TRANSPILED -- a library has no entry point, so it was\n"
            "# never RUN and never diffed against a native build. LIB_BUILT\n"
            "# says the project translates and type-checks; it says nothing\n"
            "# about whether it computes the right answers.\n"
            "#\n"
            "# The trailing '# ported k/n' comment is FR-44's per-item score,\n"
            "# advisory here and ignored by the parser: the AUTHORITATIVE\n"
            "# per-item ledger is each project's own expected-items.txt, next\n"
            "# to its sources. It is repeated on this line so the manifest\n"
            "# reads as a ranked backlog -- a REJECTED project at 6/8 items is\n"
            "# a very different backlog entry from one at 0/40.\n"
            "#\n"
            "# Regenerated via: run_realworld.py ... --manifest-format outcomes"
            " --update\n"
        )
        for entry in sorted(results):
            line = "%s %s" % (entry.name, entry.status)
            if entry.tag:
                line += " " + entry.tag
            if entry.items is not None:
                ported, portable = ported_fraction(entry.items)
                line += "  # ported %d/%d" % (ported, portable)
            handle.write(line + "\n")


def load_outcome_manifest(path):
    """Parse an outcome manifest into {name: (status, tag)}."""
    if not os.path.isfile(path):
        raise SystemExit("error: manifest not found: %s" % path)
    expected = {}
    with open(path, "r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, start=1):
            stripped = line.split("#", 1)[0].strip()
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) < 2 or fields[1] not in OUTCOMES:
                raise SystemExit("error: %s:%d: expected '<name> <OUTCOME> [tag]'"
                                 " with OUTCOME one of %s"
                                 % (path, lineno, "/".join(OUTCOMES)))
            expected[fields[0]] = (fields[1], fields[2] if len(fields) > 2 else "")
    return expected


def ratchet_outcomes(manifest_path, results):
    """Compare measured outcomes against the outcome manifest.

    Returns (failures, notices): failures fail the gate, notices are printed
    as advisory drift.
    """
    expected = load_outcome_manifest(manifest_path)
    measured = {entry.name: entry for entry in results}

    failures = []
    notices = []

    missing = sorted(set(expected) - set(measured))
    if missing:
        failures.append("%d manifest entr(ies) with no corpus project: %s."
                        " Re-run with --update after removing a project."
                        % (len(missing), ", ".join(missing)))

    unlisted = sorted(set(measured) - set(expected))
    if unlisted:
        failures.append("%d corpus project(s) missing from the manifest: %s."
                        " Re-run with --update to record the baseline."
                        % (len(unlisted), ", ".join(unlisted)))

    regressions = []
    improvements = []
    for name in sorted(set(expected) & set(measured)):
        want_status, want_tag = expected[name]
        got = measured[name]
        if got.status == want_status:
            if got.tag != want_tag:
                notices.append("%s: blocker tag drifted %r -> %r (advisory;"
                               " --update refreshes it)"
                               % (name, want_tag or "-", got.tag or "-"))
            continue
        if want_status == TRANSPILED:
            regressions.append("%s: %s -> %s" % (name, want_status, got.status))
        elif want_status == LIB_BUILT and got.status not in BUILT_OUTCOMES:
            # FR-51: a project that used to emit a library crate that COMPILES
            # and now does not has lost real ground, exactly as a lost
            # TRANSPILED has. Reaching TRANSPILED from LIB_BUILT is the one
            # move out of LIB_BUILT that is progress, and it falls through to
            # the improvement branch below.
            regressions.append("%s: %s -> %s" % (name, want_status, got.status))
        elif got.status in BUILT_OUTCOMES:
            improvements.append("%s: %s -> %s" % (name, want_status, got.status))
        else:
            # REJECTED <-> MISCOMPILE. MISCOMPILE is separately fatal unless
            # quarantined; the reverse direction is progress worth ratcheting.
            improvements.append("%s: %s -> %s" % (name, want_status, got.status))

    if regressions:
        failures.append("%d regression(s) -- a project that built no longer"
                        " does, or a differentially validated one no longer"
                        " is: %s" % (len(regressions), "; ".join(regressions)))
    if improvements:
        failures.append("%d outcome change(s) not in the manifest: %s."
                        " Re-run with --update to ratchet forward."
                        % (len(improvements), "; ".join(improvements)))
    return failures, notices


def items_manifest_path(corpus_dir, name):
    """The per-project FR-44 item ledger, beside that project's sources."""
    return os.path.join(corpus_dir, name, ITEMS_MANIFEST_NAME)


def write_items_manifest(path, name, scores):
    """Write one project's '<symbol> <kind> <status>' per-item ledger.

    Only the FR-40 item-graph items are recorded. The report's
    ``off_graph_items`` -- rejected C++ member functions, anonymous and
    block-scope records, everything ItemGraph.h documents as unmodelled -- are
    deliberately NOT ratcheted: their symbols are not unique (three classes may
    each define ``area_x100``) so the only total key includes an absolute
    source path, which cannot be checked in.
    """
    items = scores.report.get("items", [])
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(
            "# EmitRust per-item porting ledger for RealWorld C++ project"
            " '%s' (FR-44).\n" % name
            + "#\n"
            "# One line per item of the project's FR-40 item graph, as\n"
            "# '<symbol> <kind> <status>', sorted by symbol. Status is one of\n"
            "# ported / stubbed / dropped / missing / declared; see\n"
            "# tools/emitrust-cc/ProgressReport.h. Measured by\n"
            "# 'emitrust-cc --emit=crate --incremental', whose crate is also\n"
            "# built on every run -- a partially ported project must still\n"
            "# COMPILE.\n"
            "#\n"
            "# Like the outcome manifest, this is an honest baseline and a\n"
            "# backlog, not a conformance target, and the ratchet is two-way:\n"
            "# an item that stops porting fails the gate as a regression, and\n"
            "# an item that starts porting fails until it is ratcheted\n"
            "# forward. A change between two NON-ported statuses (dropped\n"
            "# <-> stubbed) is reported as advisory drift.\n"
            "#\n"
            "# Regenerated via: run_realworld.py ... --manifest-format outcomes"
            " --update\n"
        )
        for item in sorted(items, key=lambda entry: entry["symbol"]):
            handle.write("%s %s %s\n"
                         % (item["symbol"], item["kind"] or "-",
                            item["status"]))


def load_items_manifest(path):
    """Parse a per-item ledger into {symbol: (kind, status)}."""
    expected = {}
    with open(path, "r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, start=1):
            stripped = line.split("#", 1)[0].strip()
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) != 3:
                raise SystemExit("error: %s:%d: expected"
                                 " '<symbol> <kind> <status>'" % (path, lineno))
            expected[fields[0]] = (fields[1], fields[2])
    return expected


def ratchet_items(corpus_dir, results):
    """Compare each project's measured per-item scores against its ledger.

    Returns (failures, notices), matching ``ratchet_outcomes``: failures fail
    the gate, notices are advisory drift.
    """
    failures = []
    notices = []
    for entry in sorted(results):
        scores = entry.items
        if scores is None:
            failures.append("%s: no per-item score -- 'emitrust-cc"
                            " --emit=crate --incremental' produced no readable"
                            " emitrust-progress.json. Incremental mode is"
                            " supposed to survive an unsupported project."
                            % entry.name)
            continue
        if not scores.built:
            failures.append("%s: the --incremental crate does NOT build."
                            " A partially ported project must still compile;"
                            " that is the whole claim of FR-44." % entry.name)
        if scores.report.get("denominator_source") != "item-graph":
            failures.append("%s: the project item graph could not be built, so"
                            " the per-item score has no denominator." % entry.name)
            continue

        path = items_manifest_path(corpus_dir, entry.name)
        if not os.path.isfile(path):
            failures.append("%s: missing per-item ledger %s. Re-run with"
                            " --update to record the baseline."
                            % (entry.name, path))
            continue
        expected = load_items_manifest(path)
        measured = {item["symbol"]: (item["kind"] or "-", item["status"])
                    for item in scores.report.get("items", [])}

        gone = sorted(set(expected) - set(measured))
        if gone:
            failures.append("%s: %d ledger item(s) no longer in the project:"
                            " %s. Re-run with --update."
                            % (entry.name, len(gone), ", ".join(gone)))
        fresh = sorted(set(measured) - set(expected))
        if fresh:
            failures.append("%s: %d project item(s) missing from the ledger:"
                            " %s. Re-run with --update."
                            % (entry.name, len(fresh), ", ".join(fresh)))

        regressions = []
        improvements = []
        for symbol in sorted(set(expected) & set(measured)):
            want_kind, want_status = expected[symbol]
            got_kind, got_status = measured[symbol]
            if got_kind != want_kind:
                notices.append("%s: %s kind drifted %r -> %r (advisory;"
                               " --update refreshes it)"
                               % (entry.name, symbol, want_kind, got_kind))
            if got_status == want_status:
                continue
            if want_status == "ported":
                regressions.append("%s: %s -> %s"
                                   % (symbol, want_status, got_status))
            elif got_status == "ported":
                improvements.append("%s: %s -> %s"
                                    % (symbol, want_status, got_status))
            else:
                notices.append("%s: %s status drifted %s -> %s (both unported;"
                               " advisory, --update refreshes it)"
                               % (entry.name, symbol, want_status, got_status))
        if regressions:
            failures.append("%s: %d item regression(s) -- expected ported, no"
                            " longer: %s"
                            % (entry.name, len(regressions),
                               "; ".join(regressions)))
        if improvements:
            failures.append("%s: %d item(s) newly ported and not in the ledger:"
                            " %s. Re-run with --update to ratchet forward."
                            % (entry.name, len(improvements),
                               "; ".join(improvements)))
    return failures, notices


def main(argv):
    args = parse_args(argv)
    tool = resolve_tool(args.emitrust_cc)
    native_cc = resolve_tool(args.clang)
    native_std = "-std=c++17" if args.corpus_kind == "cpp" else "-std=c11"

    if not os.path.isdir(args.corpus):
        raise SystemExit("error: corpus directory not found: %s" % args.corpus)
    programs = discover_programs(args.corpus, args.corpus_kind)
    if not programs:
        raise SystemExit("error: no corpus programs found under %s" % args.corpus)

    os.makedirs(args.workdir, exist_ok=True)
    quarantined = load_name_list(args.known_miscompiles, required=False) if args.known_miscompiles else set()

    # FR-44 per-item scoring is a C++-subset instrument: the C corpus is graded
    # by outcome alone, and running the extra incremental cargo build on each
    # of its programs would only double the gate's wall clock.
    score_items = args.corpus_kind == "cpp"

    workers = min(8, os.cpu_count() or 1)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(
            lambda p: run_single_program(tool, native_cc, native_std, p,
                                         args.workdir, score_items), programs))

    transpiled = {e.name for e in results if e.status == TRANSPILED}
    rejected = [e for e in results if e.status == REJECTED]
    miscompiles = [e for e in results if e.status == MISCOMPILE]

    # Per-program report, sorted by name. The FR-44 ported fraction rides on
    # the TRANSPILED/REJECTED lines: for a rejected project it is the whole
    # point of the run, and for a transpiled one it is the regression guard
    # that says the whole project really did port.
    for entry in sorted(results):
        suffix = ("  ported=%s" % format_ported_fraction(entry.items)
                  if entry.items is not None else "")
        if entry.status == TRANSPILED:
            print("TRANSPILED  %s%s" % (entry.name, suffix))
        elif entry.status == LIB_BUILT:
            print("LIB_BUILT   %s%s  (library crate: compiled, NOT run --"
                  " no executable oracle)" % (entry.name, suffix))
        elif entry.status == REJECTED:
            print("REJECTED    %s  [%s]%s  %s"
                  % (entry.name, entry.tag, suffix, entry.detail))
        else:
            mark = "known, quarantined" if entry.name in quarantined else "NEW"
            print("MISCOMPILE (%s): %s" % (mark, entry.name))
            print("  " + entry.detail.replace("\n", "\n  "))

    # Blocker-tag tabulation (the W4.1 survey signal; for the C++ corpus, the
    # ranked backlog that sequences the next input-subset waves).
    counts = {}
    for entry in rejected:
        counts[entry.tag] = counts.get(entry.tag, 0) + 1
    if counts:
        print("\nblocker tabulation (rejected programs by tag):")
        for tag, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
            print("  %-28s %d" % (tag, count))

    failures = []
    new_miscompiles = [e for e in miscompiles if e.name not in quarantined]
    if new_miscompiles:
        failures.append(
            "%d unquarantined MISCOMPILE(s): %s -- a transpiled program produced"
            " wrong behavior; this always fails, manifest or not."
            % (len(new_miscompiles), ", ".join(e.name for e in new_miscompiles)))

    if args.update:
        if args.manifest_format == "outcomes":
            write_outcome_manifest(args.manifest, results)
            print("manifest updated: %s (%d project outcomes)"
                  % (args.manifest, len(results)))
            for entry in sorted(results):
                if entry.items is None:
                    print("note: %s has no per-item score; its %s is left"
                          " untouched" % (entry.name, ITEMS_MANIFEST_NAME))
                    continue
                path = items_manifest_path(args.corpus, entry.name)
                write_items_manifest(path, entry.name, entry.items)
                print("item ledger updated: %s (%d items, %s ported)"
                      % (path, len(entry.items.report.get("items", [])),
                         format_ported_fraction(entry.items)))
        else:
            write_name_manifest(args.manifest, transpiled)
            print("manifest updated: %s (%d transpiled)" % (args.manifest, len(transpiled)))
    elif args.manifest_format == "outcomes":
        ratchet_failures, notices = ratchet_outcomes(args.manifest, results)
        item_failures, item_notices = ratchet_items(args.corpus, results)
        for notice in notices + item_notices:
            print("note: " + notice)
        failures.extend(ratchet_failures)
        failures.extend(item_failures)
    else:
        manifest = load_name_list(args.manifest, required=True)
        # FR-51: the `names` manifest records only the differentially
        # validated set, so it cannot express LIB_BUILT and deliberately does
        # not try -- a library crate is NOT transpiling in this manifest's
        # sense. Report the fact instead of dropping it, so a C corpus program
        # that starts emitting a library crate is visible rather than silently
        # indistinguishable from a rejection.
        lib_built = sorted(e.name for e in results if e.status == LIB_BUILT)
        if lib_built:
            print("note: %d program(s) emitted a LIBRARY crate that compiles"
                  " (no main, so no differential run; not recorded in this"
                  " manifest format): %s" % (len(lib_built), ", ".join(lib_built)))
        regressions = sorted(manifest - transpiled)
        improvements = sorted(transpiled - manifest)
        if regressions:
            failures.append("%d regression(s) -- in manifest but no longer transpiling: %s"
                            % (len(regressions), ", ".join(regressions)))
        if improvements:
            failures.append("%d improvement(s) -- transpiling but not in manifest: %s."
                            " Re-run with --update to ratchet forward."
                            % (len(improvements), ", ".join(improvements)))

    # `transpiled` and `lib_built_count` are reported separately and never
    # summed: only the first has been differentially validated, and a single
    # combined "it worked" number would erase exactly the distinction FR-51
    # introduced the outcome to preserve.
    lib_built_count = sum(1 for e in results if e.status == LIB_BUILT)
    print("\nsummary: total=%d transpiled=%d lib_built=%d rejected=%d"
          " miscompiled=%d (quarantined=%d)"
          % (len(results), len(transpiled), lib_built_count, len(rejected),
             len(miscompiles), len(miscompiles) - len(new_miscompiles)))

    scored = [e for e in results if e.items is not None]
    if scored:
        corpus_ported = sum(ported_fraction(e.items)[0] for e in scored)
        corpus_items = sum(ported_fraction(e.items)[1] for e in scored)
        permille = (1000 * corpus_ported // corpus_items) if corpus_items else 0
        print("per-item (FR-44): %d/%d item(s) ported across %d scored"
              " project(s) (%d.%d%%)"
              % (corpus_ported, corpus_items, len(scored), permille // 10,
                 permille % 10))

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
