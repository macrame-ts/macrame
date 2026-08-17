#pragma once

// Compile-time access-mode deduction - how the library decides, per functor parameter, whether
// an access is a read or a write. Three tiers, one spelling rule (see "one spelling rule" in
// docs/guide.md §5): a non-generic functor is introspected (`Function_traits` -> the parameter's
// const-ness: `const T&` = read, `T&` = write, by-value / `T&&` rejected); a generic lambda is
// classified by the rvalue-bindability probe (`const auto&`/`auto&&` = read, `auto&` = write); and
// `ts::as_read_only`/`as_read_write` tags supply the mode explicitly. Every read position invokes
// the body with `const T&` (`mode_ref`), so a mutating body under a read classification is a
// compile error. Consumed by `Guarded`'s verbs, the multi-object `ts::access`/`ts::async`, and
// `Static_task_graph::add_node`.

#include "ts/access.h"
#include "ts/task.h"   // Cancellation_token

#include <tuple>
#include <type_traits>
#include <utility>

namespace ts
{
namespace detail
{

// Extracts the parameter type list of a callable's `operator()` (or a function pointer).
// Non-generic lambdas / functors / function pointers only; generic `auto&` params aren't
// introspectable. Shared by `Static_task_graph::add_node` and multi-object `ts::async` for
// per-argument access-mode deduction.
template<typename T>
struct Function_traits : Function_traits<decltype(&T::operator())> {};
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...)> { using args = std::tuple<A...>; };
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) const> { using args = std::tuple<A...>; };
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) noexcept> { using args = std::tuple<A...>; };
template<typename C, typename R, typename... A>
struct Function_traits<R(C::*)(A...) const noexcept> { using args = std::tuple<A...>; };
template<typename R, typename... A>
struct Function_traits<R(*)(A...)> { using args = std::tuple<A...>; };
template<typename R, typename... A>
struct Function_traits<R(*)(A...) noexcept> { using args = std::tuple<A...>; };

// `read_only` for a `const T&` parameter, `read_write` otherwise.
template<typename Arg>
constexpr Access async_mode_of()
{
    return std::is_const_v<std::remove_reference_t<Arg>> ? Access::read_only : Access::read_write;
}

// SFINAE-friendly introspectability: a class with a single, non-template `operator()` (or a
// function pointer/reference). A generic lambda's `operator()` is a template - taking its
// address without arguments is ill-formed - so it lands in the `false` case and access modes
// are classified by the rvalue probe below instead of `Function_traits`.
template<typename Fn, typename = void>
struct has_introspectable_call : std::false_type {};
template<typename Fn>
struct has_introspectable_call<Fn, std::void_t<decltype(&Fn::operator())>> : std::true_type {};

template<typename Fn>
inline constexpr bool introspectable_v =
    has_introspectable_call<std::remove_cvref_t<Fn>>::value
    || std::is_function_v<std::remove_pointer_t<std::remove_cvref_t<Fn>>>;

// Rvalue-bindability probe for generic functors: position `P` gets an rvalue `T&&`, every
// other position an lvalue `T&`. `auto&` cannot bind an rvalue ([temp.deduct.call]) ->
// `read_write`; `const auto&` / `auto&&` can -> `read_only`. Declaration-level only - the
// body is never instantiated by the probe, so this is exact and SFINAE-safe. (An `auto&&`
// parameter that mutates is mis-probed as a read, but Part of the same design makes that a
// compile error: read positions are invoked with `const T&`, so `auto&&` deduces `const T&`
// and the mutation fails to compile. The one undetectable residual is an `auto` by-value
// parameter - it probes as a read and copies; writes hit the copy. Documented in the guide.)
template<std::size_t P, std::size_t K, typename T>
using Probe_arg_t = std::conditional_t<P == K, T&&, T&>;

template<typename Fn, std::size_t P, typename... Ts, std::size_t... K>
constexpr bool probe_binds_rvalue(std::index_sequence<K...>)
{
    return std::invocable<Fn, Probe_arg_t<P, K, Ts>...>;
}

template<typename Fn, std::size_t P, typename... Ts>
constexpr Access probed_mode()
{
    return probe_binds_rvalue<Fn, P, Ts...>(std::index_sequence_for<Ts...>{})
        ? Access::read_only : Access::read_write;
}

// Per-position access-corrected reference: a read position hands the body `const T&`, so a
// mutating body under a read classification fails to compile (structural const-correctness,
// on top of the runtime harness); a write position hands `T&`.
template<Access M, typename T>
using Mode_ref_t = std::conditional_t<M == Access::read_only, const T&, T&>;

template<Access M, typename T>
constexpr Mode_ref_t<M, T> mode_ref(T* p) { return *p; }

// Deduced access mode of a single-object accessor `Fn` against payload `T`:
//  - introspectable (non-generic lambda / functor / function pointer): the resource
//    parameter's const-ness decides - `const T&` = read_only, `T&` = read_write. A by-value
//    or rvalue-ref resource parameter is rejected outright.
//  - generic (templated `operator()`): the rvalue probe - `const auto&`/`auto&&` = read_only,
//    `auto&` = read_write. Token-arity aware (a trailing `Cancellation_token` is allowed).
template<typename Fn, typename T>
constexpr Access accessor_mode()
{
    if constexpr (introspectable_v<Fn>)
    {
        using Args = typename Function_traits<std::decay_t<Fn>>::args;
        static_assert(std::tuple_size_v<Args> >= 1,
            "a guarded accessor must take the resource as its first parameter");
        // Short-circuit on the failed arity assert: forming `Arg0` for an empty
        // parameter list would add a second, misleading tuple-out-of-bounds cascade
        // after the message above. The dummy return only feeds a constraint that
        // then fails cleanly at the caller.
        if constexpr (std::tuple_size_v<Args> >= 1)
        {
            using Arg0 = std::tuple_element_t<0, Args>;
            static_assert(std::is_lvalue_reference_v<Arg0>,
                "a guarded-resource parameter must be `T&` or `const T&`: taking it by value copies "
                "the resource (writes hit the copy and are silently discarded), and `T&&` cannot "
                "bind the stored instance");
            return async_mode_of<Arg0>();
        }
        else
        {
            return Access::read_only;
        }
    }
    else
    {
        return (std::invocable<Fn, T&&> || std::invocable<Fn, T&&, const Cancellation_token&>)
            ? Access::read_only : Access::read_write;
    }
}

// An `async` accessor functor may, like the bare-task path, opt into cooperative
// cancellation by taking a trailing `Cancellation_token` after the access argument `A`
// (`[](T& v, Cancellation_token t){...}`). These accept either arity for a given `A`, so
// the read/write disambiguation and result deduction work whether or not the token is
// declared. `Executable::run` forwards the block's token to the token-taking body.
template<typename Fn, typename A>
concept Async_accessor = std::invocable<Fn, A> || std::invocable<Fn, A, const Cancellation_token&>;

// The accessor gates: mode classification first, invocability second - the order is
// load-bearing. `accessor_mode` classifies without ever instantiating a body
// (introspection / the rvalue probe), while `Async_accessor` probes invocability -
// and probing a generic lambda deduces its return type, which instantiates the body;
// for a mutating body probed against `const T&` that is a hard error, not a
// substitution failure. Conjunctions short-circuit, so a failed mode gate rejects
// cleanly at the caller and the probe is never evaluated for the wrong mode. The one
// spelling for every read/write accessor position (`Guarded`'s verbs,
// `Versioned::read`).
template<typename Fn, typename T>
concept Read_only_accessor = (accessor_mode<Fn, T>() == Access::read_only)
    && Async_accessor<Fn, const T&>;

template<typename Fn, typename T>
concept Read_write_accessor = (accessor_mode<Fn, T>() == Access::read_write)
    && Async_accessor<Fn, T&>;

template<typename Fn, typename A>
inline constexpr bool accessor_takes_token_v = std::invocable<Fn, A, const Cancellation_token&>;

// Result type of an accessor, picking the token-taking overload when present.
template<typename Fn, typename A, bool = accessor_takes_token_v<Fn, A>>
struct Async_result_sel { using type = std::invoke_result_t<Fn, A, const Cancellation_token&>; };
template<typename Fn, typename A>
struct Async_result_sel<Fn, A, false> { using type = std::invoke_result_t<Fn, A>; };

// Result type for the `M`-mode accessor overload, guarded twice. It is `void` (benign) unless
// the functor is invocable and actually classifies as `M` - so a rejected overload's trailing
// return type never computes `invoke_result_t`. Two hard errors hide behind an unguarded
// compute: MSVC evaluates a rejected overload's return type during overload resolution where
// clang SFINAEs it away (C2794/C2938 on a non-invocable combination), and for a generic lambda
// a deduced return type requires instantiating the body - instantiating a mutating body
// against `const T&` would hard-error inside the overload that was never going to be chosen.
// layered on purpose (not one `&&` expression): MSVC eagerly satisfies a concept-id in a
// default template argument even when the left operand of `&&` is already false, which would
// re-trigger the body instantiation the mode gate exists to prevent. Specialization makes the
// invocability probe structurally unreachable on a mode mismatch.
template<typename Fn, typename A,
         bool = std::invocable<Fn, A> || accessor_takes_token_v<Fn, A>>
struct Accessor_result_checked { using type = void; };
template<typename Fn, typename A>
struct Accessor_result_checked<Fn, A, true> : Async_result_sel<Fn, A> {};

template<typename Fn, typename T, Access M,
         bool = (accessor_mode<Fn, T>() == M)>   // gate FIRST: classifies without body instantiation
struct Accessor_result { using type = void; };
template<typename Fn, typename T, Access M>
struct Accessor_result<Fn, T, M, true> : Accessor_result_checked<Fn, Mode_ref_t<M, T>> {};

template<typename Fn, typename T, Access M>
using Accessor_result_t = typename Accessor_result<Fn, T, M>::type;

} // namespace detail
} // namespace ts
