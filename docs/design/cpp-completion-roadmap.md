### Roadmap: the C++ completion program (W2.15+)

W2.0-W2.14 opened a data-and-methods C++ subset plus a hand-recognized
STL surface (vector/string/array/pair/optional/string_view/variant, by-value
lambdas, structured bindings, ranged-for). At the time this roadmap was
written the Cpp17Suite ledger stood at 23/26 with three frontier markers
left: 00801 (copy constructor / elision sensor), 00901 (inheritance +
virtual dispatch), 00902 (exceptions). The W2.15+ waves added nine
corpus entries; W2.19b flipped 00901 (devirtualization), W2.23 flipped
00801 (guaranteed-elision copies=0), and **W2.24 flipped 00902 --
THE FRONTIER LIST IS EMPTY. Cpp17Suite stands 35/35: every corpus
entry transpiles byte-identically against its clang++ native.** The
markers did their job for two years of waves: each one fell to the
construct it was built to sense, none was ever edited to pass, and the
one MISCOMPILE quarantine file is still empty. The C++ subset's
frontier is now wherever the external demand measurement says it is,
not where this corpus can see.

Measured frontier, 2026-08-21, one probe per construct through
`build/tools/emitrust-cc --emit=rust` (every one is a LOCATED rejection --
the subset boundary is honest today, nothing silently miscompiles):

| construct | today's diagnostic |
|---|---|
| `template <typename T> T add(T,T)` | LANDED W2.15 (was `unsupported top-level declaration`) |
| `template <typename T> struct Box` | LANDED W2.16 (was `unsupported top-level declaration`) |
| user destructor `~R()` | LANDED W2.17 (non-virtual, defined in this TU) |
| `struct D : Base` | LANDED W2.18 (single, public, non-virtual) |
| `std::cout << x` | LANDED W2.22 (admitted operand set) |
| `std::make_unique<int>` | LANDED W2.21 (bare Box<T>, always-initialized) |
| `std::map<int,int>` | LANDED W2.20 (ordered keys, admitted value set) |

Wave order below is by unlock-value over risk. Templates lead because
clang has ALREADY monomorphized them: an instantiation is a concrete,
fully typed `FunctionDecl`/`ClassTemplateSpecializationDecl` with a body,
so the import is a naming and traversal problem, not a type-inference one
(AST-dump verified: `implicit_instantiation` nodes hang off the
`FunctionTemplateDecl`/`ClassTemplateDecl` with `TemplateArgument type
'int'` and a `CompoundStmt`). RAII follows because no real C++ class
survives without it, then inheritance, then the STL containers that carry
whole-program demand.

