// RUN: split-file %s %t
// RUN: not emitrust-import-c %t/array-form.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYFORM
// RUN: not emitrust-import-c %t/array-type.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYTYPE
// RUN: not emitrust-import-c %t/custom-deleter.cpp 2>&1 | FileCheck %s --check-prefix=DELETER
// RUN: not emitrust-import-c %t/const-payload.cpp 2>&1 | FileCheck %s --check-prefix=CONSTPAYLOAD
// RUN: not emitrust-import-c %t/container-payload.cpp 2>&1 | FileCheck %s --check-prefix=CONTAINERPAYLOAD
// RUN: not emitrust-import-c %t/nested-box.cpp 2>&1 | FileCheck %s --check-prefix=NESTEDBOX
// RUN: not emitrust-import-c %t/shared-ptr.cpp 2>&1 | FileCheck %s --check-prefix=SHAREDPTR
// RUN: not emitrust-import-c %t/weak-ptr.cpp 2>&1 | FileCheck %s --check-prefix=WEAKPTR
// RUN: not emitrust-import-c %t/vector-of-box.cpp 2>&1 | FileCheck %s --check-prefix=VECOFBOX
// RUN: not emitrust-import-c %t/optional-of-box.cpp 2>&1 | FileCheck %s --check-prefix=OPTOFBOX
// RUN: not emitrust-import-c %t/pair-of-box.cpp 2>&1 | FileCheck %s --check-prefix=PAIROFBOX
// RUN: not emitrust-import-c %t/map-of-box.cpp 2>&1 | FileCheck %s --check-prefix=MAPOFBOX
// RUN: not emitrust-import-c %t/array-of-box.cpp 2>&1 | FileCheck %s --check-prefix=ARROFBOX
// RUN: not emitrust-import-c %t/default-ctor.cpp 2>&1 | FileCheck %s --check-prefix=DEFAULTCTOR
// RUN: not emitrust-import-c %t/new-ctor.cpp 2>&1 | FileCheck %s --check-prefix=NEWCTOR
// RUN: not emitrust-import-c %t/move-init.cpp 2>&1 | FileCheck %s --check-prefix=MOVEINIT
// RUN: not emitrust-import-c %t/move-assign.cpp 2>&1 | FileCheck %s --check-prefix=MOVEASSIGN
// RUN: not emitrust-import-c %t/move-free.cpp 2>&1 | FileCheck %s --check-prefix=MOVEFREE
// RUN: not emitrust-import-c %t/null-assign.cpp 2>&1 | FileCheck %s --check-prefix=NULLASSIGN
// RUN: not emitrust-import-c %t/eq-null.cpp 2>&1 | FileCheck %s --check-prefix=EQNULL
// RUN: not emitrust-import-c %t/bool-test.cpp 2>&1 | FileCheck %s --check-prefix=BOOLTEST
// RUN: not emitrust-import-c %t/not-test.cpp 2>&1 | FileCheck %s --check-prefix=NOTTEST
// RUN: not emitrust-import-c %t/get-method.cpp 2>&1 | FileCheck %s --check-prefix=GETM
// RUN: not emitrust-import-c %t/release-method.cpp 2>&1 | FileCheck %s --check-prefix=RELEASEM
// RUN: not emitrust-import-c %t/reset-method.cpp 2>&1 | FileCheck %s --check-prefix=RESETM
// RUN: not emitrust-import-c %t/reset-new.cpp 2>&1 | FileCheck %s --check-prefix=RESETNEW
// RUN: not emitrust-import-c %t/swap-method.cpp 2>&1 | FileCheck %s --check-prefix=SWAPM
// RUN: not emitrust-import-c %t/struct-member.cpp 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: not emitrust-import-c %t/by-value-param.cpp 2>&1 | FileCheck %s --check-prefix=PARAM
// RUN: not emitrust-import-c %t/by-value-return.cpp 2>&1 | FileCheck %s --check-prefix=RETVAL
// RUN: not emitrust-import-c %t/global-object.cpp 2>&1 | FileCheck %s --check-prefix=GLOBALV
// RUN: not emitrust-import-c %t/array-local.cpp 2>&1 | FileCheck %s --check-prefix=ARRAYLOCAL
// RUN: not emitrust-import-c %t/bare-block.cpp 2>&1 | FileCheck %s --check-prefix=BAREBLOCK
// RUN: not emitrust-import-c %t/make-unique-statement.cpp 2>&1 | FileCheck %s --check-prefix=MKUBARE
// RUN: not emitrust-import-c %t/make-unique-deref.cpp 2>&1 | FileCheck %s --check-prefix=MKUDEREF
// RUN: not emitrust-import-c %t/arg-mismatch.cpp 2>&1 | FileCheck %s --check-prefix=ARGMISMATCH
// RUN: not emitrust-import-c %t/nsdmi-payload.cpp 2>&1 | FileCheck %s --check-prefix=NSDMI
// RUN: not emitrust-import-c %t/reference-argument.cpp 2>&1 | FileCheck %s --check-prefix=REFARG
// RUN: not emitrust-import-c %t/ctor-ambiguous.cpp 2>&1 | FileCheck %s --check-prefix=CTORAMBIG
// RUN: not emitrust-import-c %t/free-ref-deref.cpp 2>&1 | FileCheck %s --check-prefix=FREEREFDEREF
// RUN: not emitrust-import-c %t/free-ref-arrow-field.cpp 2>&1 | FileCheck %s --check-prefix=FREEREFARROW
// RUN: not emitrust-import-c %t/free-ref-star-field.cpp 2>&1 | FileCheck %s --check-prefix=FREEREFSTAR
// RUN: not emitrust-import-c %t/method-ref-payload-arg.cpp 2>&1 | FileCheck %s --check-prefix=METHODREFPAYLOAD

// W2.21 located rejections: every construct explicitly OUT of the
// std::unique_ptr surface this wave, plus the shapes that are out
// PERMANENTLY. Rejection is a feature here -- each of these would
// otherwise be either a silent behavior change (a null unique_ptr
// rendering as a live default payload, a custom deleter's user code
// never running, a moved-from observation silently dropped) or a
// DEFERRED rustc error with no source location, and this file is the
// baseline a later wave has to move deliberately.
//
// The framing every rejection below inherits: std::unique_ptr<T> maps to
// a BARE Box<T>, not Option<Box<T>>. Both images were measured
// byte-identical against clang++ -std=c++17 during the W2.21 spike, so
// the choice is cost and not correctness -- Option<Box<T>> forces an
// unwrap at every dereference an always-initialized program never
// needed, and FR-99's precedent (design.md: "the local stays the bare
// struct") is followed rather than diverged from. The price is that
// every nullable spelling has to reject, LOUDLY and by name, which is
// what the DEFAULTCTOR / NULLASSIGN / EQNULL / BOOLTEST / NOTTEST /
// RESETM sections below pin.

//--- array-form.cpp
#include <memory>
// The T[] form destroys with delete[] and its Rust counterpart would be
// `Box<[T]>`, a different representation with a different construction
// surface. The record name is unchanged (`unique_ptr`), so template
// argument 0 being a clang ArrayType is the only screen that sees it.
// ARRAYFORM: array-form.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the std::unique_ptr<T[]> array form has no Box image (Box<[T]> is a different, unspiked representation)
void use(void) { auto p = std::make_unique<int[]>(4); }

//--- array-type.cpp
#include <memory>
// The same screen reached through the TYPE rather than through
// make_unique -- the array form must be caught in mapStdLibraryType, not
// only at the construction site.
// ARRAYTYPE: array-type.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: the std::unique_ptr<T[]> array form has no Box image (Box<[T]> is a different, unspiked representation)
void use(void) { std::unique_ptr<int[]> p; }

//--- custom-deleter.cpp
#include <memory>
// A custom deleter runs USER CODE at destruction that `Box`'s own drop
// does not run at all -- a silent behavior change if admitted. The record
// name stays `unique_ptr`, so the deleter template argument is the only
// screen (the std::less model W2.20 established for map comparators).
// DELETER: custom-deleter.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr with a deleter other than std::default_delete
struct Free { void operator()(int *p) const { (void)p; } };
void use(void) { std::unique_ptr<int, Free> p; }

//--- const-payload.cpp
#include <memory>
// A const payload has no writable place image, and the deref chain the
// field/scalar places build is written in terms of a mutable borrow for
// the three write positions.
// CONSTPAYLOAD: const-payload.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr payload type 'const int' is not in the supported payload set
void use(int a) { auto p = std::make_unique<const int>(a); }

//--- container-payload.cpp
#include <memory>
#include <vector>
// The payload set is scalars and imported structs. A container payload is
// the unspiked `Box<Vec<...>>` surface the container screen refuses from
// the other direction (see VECOFBOX below); admitting one side without the
// other would make the pair silently asymmetric.
// CONTAINERPAYLOAD: container-payload.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr payload type 'class std::vector<int>' is not in the supported payload set
void use(void) { std::unique_ptr<std::vector<int>> p; }

//--- nested-box.cpp
#include <memory>
// A unique_ptr of unique_ptr would be `Box<Box<T>>`, two levels of deref
// chain with no test coverage at all.
// NESTEDBOX: nested-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr payload type 'class std::unique_ptr<int>' is not in the supported payload set
void use(void) { std::unique_ptr<std::unique_ptr<int>> p; }

//--- shared-ptr.cpp
#include <memory>
// Its OWN wording, deliberately off the generic "is not a recognized STL
// type" tail: this is a MODEL gap, not a backlog item. `Rc`/`Arc` have
// different aliasing rules (any write through them needs interior
// mutability, and `Rc` is not `Send`), and the subset has no
// representation for shared ownership at all. Moving it off the tail also
// keeps std::deque as the generic-tail marker in stl-invalid.cpp, so that
// pin does not have to move again.
// SHAREDPTR: shared-ptr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::shared_ptr has no Rust image; Rc/Arc have different aliasing and this subset has no model for shared ownership
void use(void) { std::shared_ptr<int> p; }

//--- weak-ptr.cpp
#include <memory>
// The same missing model named from the non-owning side.
// WEAKPTR: weak-ptr.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::weak_ptr has no Rust image; Rc/Arc have different aliasing and this subset has no model for shared ownership
void use(void) { std::weak_ptr<int> p; }

//--- vector-of-box.cpp
#include <memory>
#include <vector>
// THE CONTAINER SCREEN, and it is mandatory: `rustSpellingForElementType`
// admits ANY spellable element today (Vec<Vec<i32>>, Vec<Option<i32>> and
// Vec<R> all lower), so the moment std::unique_ptr returns an opaque,
// `Vec<Box<T>>` would be admitted with ZERO spike coverage -- the
// move/clone question at every element read is real and unanswered.
// Refusing a `Box<` spelling centrally covers vector, pair, optional and
// the map value in one place.
// VECOFBOX: vector-of-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::vector<class std::unique_ptr<int>> element type is not in the supported STL element set
void use(void) { std::vector<std::unique_ptr<int>> v; }

//--- optional-of-box.cpp
#include <memory>
#include <optional>
// The same central screen through W2.11's family.
// OPTOFBOX: optional-of-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::optional<class std::unique_ptr<int>> element type is not in the supported STL element set
void use(void) { std::optional<std::unique_ptr<int>> o; }

//--- pair-of-box.cpp
#include <memory>
#include <utility>
// The same central screen through W2.8's family (whose synthesized struct
// name is composed from the element spellings, so an unspellable element
// has nowhere to go).
// PAIROFBOX: pair-of-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::pair element type is not in the supported set
void use(void) { std::pair<int, std::unique_ptr<int>> p; }

//--- map-of-box.cpp
#include <memory>
#include <map>
// The same central screen through W2.20's family (a Box also fails the
// Default screen the entry().or_default() place needs).
// MAPOFBOX: map-of-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::map value type 'class std::unique_ptr<int>' is not in the supported value set
void use(void) { std::map<int, std::unique_ptr<int>> m; }

//--- array-of-box.cpp
#include <memory>
#include <array>
// std::array needs its OWN copy of the screen: it maps its element
// DIRECTLY (no rustSpellingForElementType round-trip) and it additionally
// dodges the W2.17 destructor gate, because `userDeclaredDestructor`
// strips CLANG array types and not `std::array`. Without this arm it is
// the one position that silently admits `[Box<T>; N]`.
// ARROFBOX: array-of-box.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::array<std::unique_ptr<...>, N> element type is not in the supported STL element set
void use(void) { std::array<std::unique_ptr<int>, 2> a; }

//--- default-ctor.cpp
#include <memory>
// THE NULLABILITY BOUNDARY, and the reason this wave admits only the
// always-initialized subset. A default-constructed unique_ptr IS null and
// `p == nullptr` is a legal, common test; a Rust `Box<T>` has no null
// state at all. `Option<Box<T>>` was measured byte-identical during the
// spike and is a viable later wave; what is NOT acceptable is admitting
// this shape onto the bare Box image, where the place would silently
// render a live default payload where C++ had none.
// DEFAULTCTOR: default-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so a default-constructed std::unique_ptr has no image
void use(void) { std::unique_ptr<int> p; }

//--- new-ctor.cpp
#include <memory>
// Adopting a raw `new` expression is a DIFFERENT shape from the null one
// above and says so: CXXNewExpr is not modeled anywhere in the importer,
// so there is nothing to adopt.
// NEWCTOR: new-ctor.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: this std::unique_ptr initializer shape is not supported (only std::make_unique<T>(args) is recognized)
void use(int a) { std::unique_ptr<int> p(new int(a)); }

//--- move-init.cpp
#include <memory>
// THE MOVE BOUNDARY. Rust's move semantics match unique_ptr's exactly in
// one direction and not the other: C++ leaves the moved-from unique_ptr
// NULL and READABLE (`q = std::move(p); if (!p) ...` is well-defined and
// prints something), while Rust cannot read a moved-from binding at all.
// Since the bare Box image has no null state, the moved-from OBSERVATION
// has no representation, so the transfer rejects rather than silently
// dropping an observable. Note the wording is unique_ptr's own, not
// W2.3's inaccurate "std::vector/std::string copy/move construction".
// MOVEINIT: move-init.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a moved-from std::unique_ptr is null and testable, but Rust cannot read a moved-from binding
void use(int a) { auto p = std::make_unique<int>(a); std::unique_ptr<int> q = std::move(p); }

//--- move-assign.cpp
#include <memory>
// The same boundary through operator= rather than the move constructor.
// MOVEASSIGN: move-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a moved-from std::unique_ptr is null and testable, but Rust cannot read a moved-from binding
void use(int a) { auto p = std::make_unique<int>(a); auto q = std::make_unique<int>(a); q = std::move(p); }

//--- move-free.cpp
#include <memory>
// `std::move` intercepted as a std-namespace FREE function, the way W2.14
// intercepted `std::get` -- otherwise the xvalue CallExpr reaches
// emitLValue and reports the AST node class instead of the reason.
// MOVEFREE: move-free.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a moved-from std::unique_ptr is null and testable, but Rust cannot read a moved-from binding
void sink(int);
void use(int a) { auto p = std::make_unique<int>(a); sink(*std::move(p)); }

//--- null-assign.cpp
#include <memory>
// Emptying a live unique_ptr. In C++ this destroys the payload AT THE
// ASSIGNMENT and leaves a null, testable object; dropping a Box early is
// expressible in Rust but the resulting binding is unusable, which is not
// the same observable.
// NULLASSIGN: null-assign.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so assigning nullptr to a std::unique_ptr has no image
void use(int a) { auto p = std::make_unique<int>(a); p = nullptr; }

//--- eq-null.cpp
#include <memory>
// `p == nullptr` is an ADL FREE operator== in namespace std, not a member,
// so it is intercepted in emitCall's operator block: the member dispatch
// never sees it and the identifier-named free dispatch cannot (an operator
// has no identifier name), which would otherwise report the generic
// "unsupported callee".
// EQNULL: eq-null.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so comparing a std::unique_ptr against nullptr has no image
extern "C" int printf(const char *, ...);
void use(int a) { auto p = std::make_unique<int>(a); printf("%d\n", (int)(p == nullptr)); }

//--- bool-test.cpp
#include <memory>
// `if (p)` is a CK_UserDefinedConversion wrapping the `operator bool`
// member call, so the CAST rejects before any member dispatch runs; the
// screen sits there so the diagnostic names the boundary instead of the
// cast kind.
// BOOLTEST: bool-test.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so testing a std::unique_ptr for emptiness has no image
extern "C" int printf(const char *, ...);
void use(int a) { auto p = std::make_unique<int>(a); if (p) printf("y\n"); }

//--- not-test.cpp
#include <memory>
// The negated spelling takes the identical conversion.
// NOTTEST: not-test.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so testing a std::unique_ptr for emptiness has no image
extern "C" int printf(const char *, ...);
void use(int a) { auto p = std::make_unique<int>(a); if (!p) printf("y\n"); }

//--- get-method.cpp
#include <memory>
// get() hands out a RAW POINTER into the Box. A raw `T *` bound to a local
// is scalarized away entirely in this model (measured: the emitted Rust
// contains no pointer at all), so there is nothing to hand back. The
// screen sits at the pointer LOCAL because the pointer planner claims the
// declaration long before emitStlMemberCall runs and would otherwise
// report its generic "pointer assigned a non-address value".
// GETM: get-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr::get() hands out a raw pointer to the payload, which has no place in this model
void use(int a) { auto p = std::make_unique<int>(a); int *q = p.get(); (void)q; }

//--- release-method.cpp
#include <memory>
// release() is get() plus a LEAK: the caller owns the payload and must
// free it. `Box::into_raw` exists but there is no `free` in the subset to
// pair it with.
// RELEASEM: release-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr::release() hands out a raw pointer to the payload, which has no place in this model
void use(int a) { auto p = std::make_unique<int>(a); int *q = p.release(); (void)q; }

//--- reset-method.cpp
#include <memory>
// reset() destroys the payload and leaves the object NULL -- the
// nullability boundary again, reached through the method table.
// RESETM: reset-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so std::unique_ptr::reset() has no image
void use(int a) { auto p = std::make_unique<int>(a); p.reset(); }

//--- reset-new.cpp
#include <memory>
// reset(new T(..)) replaces the payload, which a Rust `*p = T` could
// express -- but the interim state is still the null one, and CXXNewExpr
// is not modeled anywhere in the importer. Both spellings separate by the
// existing CXXDefaultArgExpr-as-absent convention and both are out.
// RESETNEW: reset-new.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: a Box<T> cannot be null, so std::unique_ptr::reset() has no image
void use(int a) { auto p = std::make_unique<int>(a); p.reset(new int(a)); }

//--- swap-method.cpp
#include <memory>
// `std::mem::swap` IS expressible, but it was never byte-diffed on the
// bare Box image during the spike, so it stays out with the generic
// per-method wording (which names the entity, so the ledger can still
// count it).
// SWAPM: swap-method.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::unique_ptr::swap is not a recognized STL method
void use(int a) { auto p = std::make_unique<int>(a); auto q = std::make_unique<int>(a); p.swap(q); }

//--- struct-member.cpp
#include <memory>
// THE SHARED W2.17 GATE, pinned deliberately rather than by accident. The
// wording is not unique_ptr-specific and it is not supposed to be: the
// measurement that shaped this wave is that std::vector<int> (shipped by
// W2.3) produces the BYTE-IDENTICAL diagnostic in this position and in
// the four below. std::unique_ptr inherits exactly the local-only
// boundary vector/string/map already live under; W2.21 does NOT touch
// `userDeclaredDestructor`, so these five wordings stay shared and
// accurate.
// MEMBER: struct-member.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: struct member of a class with a destructor
struct Holder { std::unique_ptr<int> p; };
int use(Holder *h) { (void)h; return 0; }

//--- by-value-param.cpp
#include <memory>
// Passing a unique_ptr by value is a MOVE at the call site, which is the
// MOVEINIT boundary reached through a signature.
// PARAM: by-value-param.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
int use(std::unique_ptr<int> p) { return 0; }

//--- by-value-return.cpp
#include <memory>
// Returning a unique_ptr is blocked at the SIGNATURE, before any W2.21
// code runs -- the same place returning a std::vector is blocked. This is
// the wave-reshaping measurement: it is not a unique_ptr gap.
// RETVAL: by-value-return.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: class with a destructor passed or returned by value
std::unique_ptr<int> use(int a) { return std::make_unique<int>(a); }

//--- global-object.cpp
#include <memory>
// A Rust `static` never drops, so a module-level Box would never run the
// payload's destructor at all.
// GLOBALV: global-object.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: global or static object of a class with a destructor
std::unique_ptr<int> g;

//--- array-local.cpp
#include <memory>
// C++ destroys an array in REVERSE index order and Rust in forward order,
// so the drop timing would differ observably.
// ARRAYLOCAL: array-local.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: array of a class with a destructor
void use(void) { std::unique_ptr<int> a[3]; }

//--- bare-block.cpp
#include <memory>
// `checkDropLocalScope` restricts a Drop-carrying local to the scopes
// whose Rust drop point provably equals the C++ destructor point. This
// arm is CORRECT for a Box and comes for free -- which is why the corpus
// entry observes drop timing from a loop body and a branch body rather
// than from a bare block.
// BAREBLOCK: bare-block.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: object of a class with a destructor outside a function, loop, or branch body
void use(int a) { { auto p = std::make_unique<int>(a); } (void)a; }

//--- make-unique-statement.cpp
#include <memory>
// A unique_ptr TEMPORARY has no binding to own its Box and therefore no
// drop point to place. Without this screen the shape falls to the generic
// "unsupported expression: CXXBindTemporaryExpr" tail, which names the
// AST node instead of the reason.
// MKUBARE: make-unique-statement.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::make_unique is only recognized as the initializer of a local std::unique_ptr variable
void use(int a) { std::make_unique<int>(a); }

//--- make-unique-deref.cpp
#include <memory>
// The same temporary reached through a place position.
// MKUDEREF: make-unique-deref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::make_unique is only recognized as the initializer of a local std::unique_ptr variable
void sink(int);
void use(int a) { sink(*std::make_unique<int>(a)); }

//--- arg-mismatch.cpp
#include <memory>
// make_unique's parameter is a FORWARDING REFERENCE (`Args&&`), so no
// conversion is materialized in the AST at all -- the argument arrives at
// its own type. Widening it here would invent a conversion the C++ program
// never wrote, so the mismatch rejects.
// ARGMISMATCH: arg-mismatch.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::make_unique argument type does not match the std::unique_ptr payload
void use(int a) { auto p = std::make_unique<long>(a); }

//--- nsdmi-payload.cpp
#include <memory>
// `make_unique<T>()` on a class with no user-declared constructor
// VALUE-initializes, which is what the `T::default()` step already
// produces -- unless an in-class member initializer makes the two differ,
// which is exactly this shape.
// NSDMI: nsdmi-payload.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::make_unique of a class with in-class member initializers and no constructor
struct Seeded { int a = 7; };
void use(void) { auto p = std::make_unique<Seeded>(); }

//--- reference-argument.cpp
#include <memory>
// A reference PARAMETER needs a second borrow live across the Box's own
// auto-deref borrow at the call; out of subset this wave.
// REFARG: reference-argument.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: reference argument to a method called through a std::unique_ptr
struct Cell { int v; Cell(int x) : v(x) {} ~Cell() {} void into(int &out) const { out = v; } };
void use(int a) { auto p = std::make_unique<Cell>(a); int z = 0; p->into(z); }

//--- ctor-ambiguous.cpp
#include <memory>
// The instantiated `std::make_unique<T, Args...>` body is never imported,
// so the payload's constructor is resolved here by ARITY over the
// user-declared, non-copy/move set. An overload set that arity cannot
// resolve rejects rather than guessing which one C++ overload resolution
// picked.
// CTORAMBIG: ctor-ambiguous.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: std::make_unique constructor overload of 'R' is ambiguous by arity
struct R {
  int v;
  R(int x) : v(x) {}
  R(float x) : v((int)x) {}
  ~R() {}
};
void use(int a) { auto p = std::make_unique<R>(a); }

//--- free-ref-deref.cpp
#include <memory>
// FR-188. A MUTABLE reference argument bound to a std::unique_ptr payload
// place. The payload borrow a READ takes is the SHARED
// `Deref::deref` one (two live `&mut` borrows of one Box in one
// expression is rustc E0499, so reads must stay shared -- see
// stl-unique-ptr.cpp), and a `&mut` argument re-borrowed out of that
// shared borrow is rustc E0596 "cannot borrow as mutable, as it is behind
// a `&` reference". Before FR-188 this shape imported and emitted a crate
// that exited 0 and did not compile, which is the FR-140/141/142/146
// silent-unbuildable-output class this repo treats as a defect.
//
// It REJECTS rather than widening the borrow to `DerefMut` because the
// callee's parameter mutability is what would have to drive that choice,
// and the `const Node &` sibling in stl-unique-ptr-ref-argument.cpp must
// keep the shared borrow -- so the widening is its own increment, and the
// safe failure direction meanwhile is a located diagnostic.
// FREEREFDEREF: free-ref-deref.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutable reference argument borrowed from a std::unique_ptr payload
struct Node { int id; int tag; };
static void h(Node &n) { n.id += 1; }
void use(int a) { auto p = std::make_unique<Node>(); p->id = a; h(*p); }

//--- free-ref-arrow-field.cpp
#include <memory>
// FR-188, the FIELD spelling of the same defect: `p->id` is the identical
// shared-`Deref::deref` payload place, so a `&mut` argument out of it is
// the same E0596. Pinned separately from the whole-payload leg above
// because the two reach the borrow through different AST shapes (a
// MemberExpr over the arrow operator call, versus the operator call
// itself), and a predicate that saw only one of them would leave the
// other silently emitting an unbuildable crate.
// FREEREFARROW: free-ref-arrow-field.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutable reference argument borrowed from a std::unique_ptr payload
struct Node { int id; int tag; };
static void hi(int &x) { x += 1; }
void use(int a) { auto p = std::make_unique<Node>(); p->id = a; hi(p->id); }

//--- free-ref-star-field.cpp
#include <memory>
// FR-188, and the WORST leg of the three: `(*p).id` is a NON-arrow
// MemberExpr whose base is the `*p` operator call, a shape the payload
// place predicate did not recognize at all. It therefore did not even
// produce the E0596 the two legs above do -- it loaded the whole payload
// into a fresh staged local and borrowed a FIELD OF THE COPY, so the
// crate BUILT CLEAN and threw the callee's write away. Measured on the
// FR-188 repro: clang++ printed `id=101` and the emitted crate printed
// `id=1`. That is a silent miscompile, which `cargo build` success cannot
// see, so this leg is the reason the rejection keys on the AST place root
// rather than on the emitted borrow.
// FREEREFSTAR: free-ref-star-field.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutable reference argument borrowed from a std::unique_ptr payload
struct Node { int id; int tag; };
static void hi(int &x) { x += 1; }
void use(int a) { auto p = std::make_unique<Node>(); p->id = a; hi((*p).id); }

//--- method-ref-payload-arg.cpp
#include <memory>
// FR-188: a payload place passed by mutable reference to an ordinary
// C++ METHOD on some other object takes the same borrow and the same
// rejection. This is NOT the pre-existing reference-argument-to-a-
// method-called-through-the-unique_ptr pin above (`p->into(z)`, the
// REFARG leg): there the unique_ptr is the RECEIVER, here it is the
// ARGUMENT, and the two must stay distinguishable because they name
// different future work.
// METHODREFPAYLOAD: method-ref-payload-arg.cpp:{{[0-9]+}}:{{[0-9]+}}: error: unsupported: mutable reference argument borrowed from a std::unique_ptr payload
struct Node { int id; int tag; };
struct Sink { int seen; void take(Node &n) { seen = n.id; } };
void use(int a) {
  auto p = std::make_unique<Node>();
  p->id = a;
  Sink s;
  s.take(*p);
}
