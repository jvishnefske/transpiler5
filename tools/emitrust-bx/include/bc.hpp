// bc.hpp -- a compile-time borrow discipline for C++20, built from three facts:
//  (1) every lambda expression has a unique type, so `auto Brand = []{}` gives each
//      Owner declaration its own type -- object identity becomes TYPE identity;
//  (2) a captureless lambda cannot odr-use enclosing locals, so a borrow callback
//      cannot reach any other Owner while it holds a reference;
//  (3) a borrow lives exactly as long as the callback, and the callback's result
//      must be borrow-free, so no reference can escape it.
// `bc::with(a, bc::ref(b), [](auto& x, const auto& y) {..})` is the C++ spelling of
// `{ let x = &mut a; let y = &b; .. }` (FR-255).
//
// KNOWN UNSOUNDNESS -- the discipline is a lint built from types, not a proof:
//  - inside one borrow, T& is plain C++: iterator invalidation WITHIN one container is
//    not caught (wrap the finer-grained parts in their own Owners);
//  - static and global Owners are reachable from a captureless callback;
//  - a callback can stash &x in a global or static by side effect;
//  - bc::mut(o) / bc::ref(o) are plain references: stored in a variable they can
//    outlive o or a std::move of o (use-after-move is left to clang-tidy
//    bugprone-use-after-move);
//  - bc::detail is a convention, not a boundary: calling into it opens an Owner
//    without the checker.
// Each written-out `bc::Owner<X>` is a NEW type (Clang and GCC both give a lambda in a
// default template argument a fresh type per use), so name a nested owner type ONCE
// with `using`. Same-type Owners are conservatively treated as one object: borrowing
// two elements of a std::vector<Owner<..>> together is rejected, like Rust indexing.
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bc {

// ---- borrow-free types: values that cannot carry a reference out of a borrow ----
template <class T, class = void> struct borrow_free : std::false_type {};
template <class T>
struct borrow_free<T, std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>>
    : std::true_type {};
template <> struct borrow_free<void> : std::true_type {};
template <> struct borrow_free<std::string> : std::true_type {};
template <class T, class A> struct borrow_free<std::vector<T, A>> : borrow_free<T> {};
template <class T> struct borrow_free<std::optional<T>> : borrow_free<T> {};
template <class T, std::size_t N> struct borrow_free<std::array<T, N>> : borrow_free<T> {};
template <class A, class B>
struct borrow_free<std::pair<A, B>> : std::bool_constant<borrow_free<A>::value && borrow_free<B>::value> {};
template <class... Ts>
struct borrow_free<std::tuple<Ts...>> : std::bool_constant<(borrow_free<Ts>::value && ...)> {};
// User aggregates opt in by listing their fields' types (C++20 has no reflection):
//   BC_BORROW_FREE(Point, int, int)
template <class... Ts> struct all_borrow_free : std::bool_constant<(borrow_free<Ts>::value && ...)> {};
#define BC_BORROW_FREE(Type, ...) \
  template <> struct bc::borrow_free<Type> : bc::all_borrow_free<__VA_ARGS__> {}
template <class T> inline constexpr bool borrow_free_v = borrow_free<std::remove_cv_t<T>>::value;

template <class T, auto Brand> class Owner;
namespace detail { struct opener; }
template <class T> struct is_owner : std::false_type {};
template <class T, auto B> struct is_owner<Owner<T, B>> : std::true_type {};
// Owners compose: an Owner is itself borrow-free, so it can live in a vector, a pair,
// or another Owner.
template <class T, auto B> struct borrow_free<Owner<T, B>> : borrow_free<T> {};

// ---- the owner: sole holder of a T; no copy, no direct access ----
template <class T, auto Brand = [] {}> class Owner {
  static_assert(borrow_free_v<T>, "bc::Owner<T>: T may contain a reference or pointer");
  T value_;
  friend struct detail::opener;  // the ONLY way in: bc::with, through the checker
public:
  using value_type = T;
  static constexpr auto brand = Brand;
  template <class... A>
    requires(!(sizeof...(A) == 1 && (std::is_same_v<std::remove_cvref_t<A>, Owner> && ...)))
  explicit Owner(A&&... a) : value_(std::forward<A>(a)...) {}
  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;
  Owner(Owner&&) = default;
  Owner& operator=(Owner&&) = default;
  Owner clone() const { return Owner(value_); }
};

// ---- access modes ----
template <class O> struct Mut { O& o; };
template <class O> struct Ref { const O& o; };
template <class T, auto B> Mut<Owner<T, B>> mut(Owner<T, B>& o) { return {o}; }
template <class T, auto B> Ref<Owner<T, B>> ref(const Owner<T, B>& o) { return {o}; }

namespace detail {
template <class X> struct mode;
template <class O> struct mode<Mut<O>> { using owner = O; static constexpr bool is_mut = true; };
template <class O> struct mode<Ref<O>> { using owner = O; static constexpr bool is_mut = false; };
template <class T, auto B> struct mode<Owner<T, B>> : mode<Mut<Owner<T, B>>> {};  // bare = mut
template <class X> using owner_t = typename mode<std::remove_cvref_t<X>>::owner;

// Aliasing XOR mutation, decided on TYPES: a mutable borrow's owner type must not
// appear anywhere else in the borrow list.
template <class X, class Y> constexpr bool conflict() {
  return std::is_same_v<owner_t<X>, owner_t<Y>> &&
         (mode<std::remove_cvref_t<X>>::is_mut || mode<std::remove_cvref_t<Y>>::is_mut);
}
template <class X, class... Ys> constexpr bool none_conflict() {
  return (!conflict<X, Ys>() && ...);
}
template <class... Xs> struct all_disjoint : std::true_type {};
template <class X, class... Ys>
struct all_disjoint<X, Ys...>
    : std::bool_constant<none_conflict<X, Ys...>() && all_disjoint<Ys...>::value> {};

struct opener {
  template <class O> static auto& open(Mut<O> m) { return m.o.value_; }
  template <class O> static const auto& open(Ref<O> r) { return r.o.value_; }
  template <class T, auto B> static T& open(Owner<T, B>& o) { return o.value_; }
};
}  // namespace detail

// with(borrows..., f): open every borrow for the duration of f, nothing longer.
template <class... Args> decltype(auto) with(Args&&... args) {
  auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
  constexpr std::size_t N = sizeof...(Args) - 1;
  auto&& f = std::get<N>(tup);
  using F = std::remove_cvref_t<decltype(f)>;
  static_assert(std::is_empty_v<F> && std::is_default_constructible_v<F>,
                "bc::with: the callback must be a captureless lambda -- pass every "
                "Owner it needs as a borrow, so the checker can see it");
  return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
    static_assert(detail::all_disjoint<std::tuple_element_t<I, decltype(tup)>...>::value,
                  "bc::with: an Owner borrowed mutably is also borrowed elsewhere "
                  "in this call (aliasing XOR mutation)");
    using R = decltype(f(detail::opener::open(std::get<I>(tup))...));
    static_assert(!std::is_reference_v<R> && !std::is_pointer_v<R> &&
                      borrow_free_v<std::remove_cv_t<R>>,
                  "bc::with: the callback's result may carry a borrow out of its scope");
    return f(detail::opener::open(std::get<I>(tup))...);
  }(std::make_index_sequence<N>{});
}

// with_two(v, i, j, f): two MUTABLE borrows of distinct elements of one vector --
// what the type-level check must refuse (same element type), discharged by a
// RUNTIME check instead, like Rust's `v.get_disjoint_mut([i, j])` / split_at_mut.
// Elements that are Owners are opened, so f sees their values. i == j or an index
// out of range aborts loudly (Rust would panic); it never returns two aliases.
template <class E, class A, class F>
decltype(auto) with_two(std::vector<E, A>& v, std::size_t i, std::size_t j, F f) {
  static_assert(std::is_empty_v<F> && std::is_default_constructible_v<F>,
                "bc::with_two: the callback must be a captureless lambda");
  if (i == j || i >= v.size() || j >= v.size()) {
    std::fprintf(stderr, "bc::with_two: indices %zu and %zu of a vector of %zu do not name "
                 "two distinct elements\n", i, j, v.size());
    std::abort();
  }
  auto open = [](E& e) -> auto& {
    if constexpr (is_owner<E>::value) return detail::opener::open(e);
    else return e;
  };
  using R = decltype(f(open(v[i]), open(v[j])));
  static_assert(!std::is_reference_v<R> && !std::is_pointer_v<R> &&
                    borrow_free_v<std::remove_cv_t<R>>,
                "bc::with_two: the callback's result may carry a borrow out of its scope");
  return f(open(v[i]), open(v[j]));
}

}  // namespace bc
