#pragma once

#include <clean-core/common/macros.hh> // CC_PURE on make_hash_of_bytes
#include <clean-core/fwd.hh>           // cc::span is used only by-declaration below; span.hh would cycle (it hashes)
#include <clean-core/math/bit.hh>
#include <clean-core/math/wide_arith.hh>

#include <type_traits>

// The hashing scheme used by hash maps/sets.
//
// A type opts in through one of two tiers: a `cc::custom::hash_trait<T>` specialization (the override tier, checked first),
// or an ADL hidden friend `friend u64 hash(T const&)`, which is the default for types you own.
// A type should provide exactly one of the two.
// [customization-points](../../../docs/customization-points.md) owns that protocol, the tier order and the reasoning behind it.
//
// `cc::hash` as a free function is intentionally NEVER defined: the unqualified `hash(v)` in impl::hash_one must resolve to ADL hidden friends only.
//
// The quality contract is this header's own:
//   `make_hash` is the COMPOSABLE building block and does not finalize, so composite types fold their members through `combine_hash` without re-scrambling at every level.
//   `make_hash_finalized` adds one bijective avalanche, and is what a stand-alone hash uses — a single key, or a container's own hash.
//   Hash tables therefore consume FINALIZED hashes and just mask low bits; they never re-scramble.
//   That keeps finalization at O(1) per key instead of compounding with nesting depth.

namespace cc::custom
{
/// Override-tier customization point for cc::make_hash: specialize it with `static u64 hash(T const&)`.
/// The primary template has no `hash` member, so types without a specialization fall through to their ADL hidden friend.
template <class T>
struct hash_trait
{
};

// --- built-in hashes (composable, i.e. deliberately un-finalized) ---------------------------------

/// Integers and enums hash to their zero-extended value — cheap and composable.
/// The avalanche is applied later by combine_hash / make_hash_finalized, never here.
template <class T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
struct hash_trait<T>
{
    [[nodiscard]] static constexpr u64 hash(T v)
    {
        if constexpr (std::is_same_v<T, bool>)
            return v ? 1u : 0u;
        else if constexpr (std::is_enum_v<T>)
        {
            using U = std::underlying_type_t<T>;
            return static_cast<u64>(static_cast<std::make_unsigned_t<U>>(static_cast<U>(v)));
        }
        else
            return static_cast<u64>(static_cast<std::make_unsigned_t<T>>(v));
    }
};

/// Floats hash by bit pattern.
/// Adding +0.0 collapses -0.0 to +0.0 so the two zeros hash equally, and is the identity on every other value — branchless, and constexpr.
/// NaNs keep a NaN bit pattern, which costs nothing: NaN != NaN anyway, so the equal-implies-equal-hash invariant is not at stake.
template <>
struct hash_trait<float>
{
    [[nodiscard]] static constexpr u64 hash(float v) { return u64(cc::bit_cast<u32>(v + 0.0f)); }
};
template <>
struct hash_trait<double>
{
    [[nodiscard]] static constexpr u64 hash(double v) { return cc::bit_cast<u64>(v + 0.0); }
};

/// Pointers hash by address (not constexpr — addresses are not constant-evaluable).
template <class T>
struct hash_trait<T*>
{
    [[nodiscard]] static u64 hash(T* p) { return reinterpret_cast<u64>(p); }
};
template <>
struct hash_trait<nullptr_t>
{
    [[nodiscard]] static constexpr u64 hash(nullptr_t) { return 0; }
};
} // namespace cc::custom

namespace cc
{
// --- mixers ---------------------------------------------------------------------------------------

/// Bijective 64-bit avalanche (moremur), used to FINALIZE a single value.
/// Being a permutation, it adds no collisions of its own.
/// Pure 64-bit multiplies, so it needs nothing from the 128-bit path.
[[nodiscard]] constexpr u64 hash_finalize(u64 x)
{
    x ^= x >> 27;
    x *= 0x3C79AC492BA7B653ull;
    x ^= x >> 33;
    x *= 0x1C69B3F74AC4AE35ull;
    x ^= x >> 27;
    return x;
}

/// Order-dependent 2->1 join (wyhash-style 128-bit multiply fold).
/// Strongly avalanching even on low-entropy inputs, and the distinct non-zero constants keep the all-zero input off the multiply's fixed point.
/// Non-bijective by nature, since it reduces 128 bits to 64 — which is exactly right for a join.
[[nodiscard]] constexpr u64 combine_hash(u64 a, u64 b)
{
    u128 const p = cc::umul128(a ^ 0x2d358dccaa6c78a5ull, b ^ 0x8bb84b93962eacc9ull);
    return p.lo ^ p.hi;
}

/// Order-INDEPENDENT join for content-keyed containers (sets, maps).
/// Commutative and associative, so the result does not depend on iteration order.
/// It composes as a monoid: partial accumulators merge by combining again, and you finalize the total once.
/// Inputs MUST already be finalized via make_hash_finalized — addition is linear, so raw element hashes would cancel ({1,4} and {2,3} both sum to 5).
[[nodiscard]] constexpr u64 combine_hash_unordered(u64 a, u64 b)
{
    return a + b;
}

/// 64-bit XXH3 hash of a byte range, the 64-bit sibling of hash128::create.
/// Stable for a given (data, seed); a seed of 0 selects XXH3's unseeded variant.
/// The workhorse behind byte-range hashes such as strings — <xxhash.h> stays private to hash.cc.
[[nodiscard]] CC_PURE u64 make_hash_of_bytes(cc::span<byte const> data, u64 seed = 0);

// --- make_hash / make_hash_finalized --------------------------------------------------------------

namespace impl
{
template <class>
inline constexpr bool dependent_false = false;

template <class T>
[[nodiscard]] constexpr u64 hash_one(T const& v)
{
    if constexpr (requires { cc::custom::hash_trait<T>::hash(v); })
        return cc::custom::hash_trait<T>::hash(v); // override tier
    else if constexpr (requires { hash(v); })
        return hash(v); // ADL hidden friend (unqualified -> ADL only)
    else
        static_assert(dependent_false<T>, "cc::make_hash: no hash for T — add a 'friend u64 hash(T const&)' or "
                                          "specialize cc::hash_trait<T>");
}

struct make_hash_fn
{
    /// Composable hash of one or more values.
    /// A single argument gives that element's un-finalized hash.
    /// Several arguments give an ordered combine_hash fold over each element's hash.
    template <class T, class... Rest>
    [[nodiscard]] constexpr u64 operator()(T const& v, Rest const&... rest) const
    {
        u64 h = hash_one(v);
        ((h = cc::combine_hash(h, hash_one(rest))), ...);
        return h;
    }
};

struct make_hash_finalized_fn
{
    /// make_hash followed by one bijective avalanche — the stand-alone form a hash table consumes.
    template <class T, class... Rest>
    [[nodiscard]] constexpr u64 operator()(T const& v, Rest const&... rest) const
    {
        return cc::hash_finalize(make_hash_fn{}(v, rest...));
    }
};
} // namespace impl

/// Composable hash (no finalize) — the customization-point entry.
/// A niebloid, so it cannot be ADL-hijacked, and it can be passed as a hasher.
inline constexpr impl::make_hash_fn make_hash = {};

/// Finalized hash = hash_finalize(make_hash(...)). Use for stand-alone keys and container hashes.
inline constexpr impl::make_hash_finalized_fn make_hash_finalized = {};

/// Default transparent hasher for the node-chaining associative containers (cc::map / cc::set).
/// Finalizes via make_hash_finalized, so tables can mask low bits directly.
/// Transparent: K and heterogeneous probe keys hash through the same path, so equal keys of different types (string / string_view) hash equally.
/// That is the precondition for heterogeneous lookup.
struct default_hash
{
    template <class T>
    [[nodiscard]] constexpr u64 operator()(T const& v) const
    {
        return cc::make_hash_finalized(v);
    }
};

/// Structural, ORDER-dependent hash of a range — the building block for the sequence containers (vector, array, span, …).
/// Folds each element's make_hash through combine_hash, and mixes in the element count so different lengths, and the empty range, stay distinct.
/// Composable, not finalized.
template <class Range>
[[nodiscard]] constexpr u64 make_hash_range(Range const& range)
{
    u64 h = 0;
    u64 n = 0;
    for (auto const& e : range)
    {
        h = cc::combine_hash(h, cc::make_hash(e));
        ++n;
    }
    return cc::combine_hash(h, n);
}

/// Structural, ORDER-INDEPENDENT hash of a range, for content-keyed containers such as set and map.
/// Sums each element's finalized hash — commutative, so iteration order does not matter — then mixes in the count.
/// Elements are finalized first because addition is linear, and raw hashes would cancel.
/// Composable, not finalized.
template <class Range>
[[nodiscard]] constexpr u64 make_hash_range_unordered(Range const& range)
{
    u64 acc = 0;
    u64 n = 0;
    for (auto const& e : range)
    {
        acc = cc::combine_hash_unordered(acc, cc::make_hash_finalized(e));
        ++n;
    }
    return cc::combine_hash(acc, n);
}
} // namespace cc
