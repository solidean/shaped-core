#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/fwd.hh>

// Comparator vocabulary: what the ordering algorithms in clean-core/algorithm/ take as their `compare` argument.
//
// A comparator here must be a STRICT WEAK ORDERING over the values it sees.
// The sorts rely on that for termination and assert when it is violated, so `<=` where `<` was meant, or a float
// that can be NaN, is a contract violation rather than a slow sort.

/// Default transparent ordering: a < b.
/// Heterogeneous when the types define operator< against each other.
struct cc::default_less
{
    template <class A, class B>
    [[nodiscard]] constexpr bool operator()(A const& a, B const& b) const
    {
        return a < b;
    }
};

/// The reversed default ordering, i.e. what sorts descending.
/// Spelled b < a so that only operator< is required, matching default_less.
struct cc::default_greater
{
    template <class A, class B>
    [[nodiscard]] constexpr bool operator()(A const& a, B const& b) const
    {
        return b < a;
    }
};

/// A projection cc::compare_by should order by in reverse.
/// Built by cc::descending, never spelled directly, and not itself a comparator — it only means something
/// inside a compare_by.
template <class ProjF>
struct cc::descending_projection
{
    ProjF projection;
};

namespace cc::impl
{
template <class T>
inline constexpr bool is_descending_projection = false;
template <class ProjF>
inline constexpr bool is_descending_projection<cc::descending_projection<ProjF>> = true;

/// One projection's verdict: -1 when a belongs first, +1 when b does, 0 when they are equivalent under it.
template <class ProjF, class A, class B>
[[nodiscard]] constexpr int compare_projection(ProjF const& projection, A const& a, B const& b)
{
    if constexpr (impl::is_descending_projection<ProjF>)
    {
        auto const& key_a = cc::invoke(projection.projection, a);
        auto const& key_b = cc::invoke(projection.projection, b);
        if (key_b < key_a)
            return -1;
        if (key_a < key_b)
            return 1;
        return 0;
    }
    else
    {
        auto const& key_a = cc::invoke(projection, a);
        auto const& key_b = cc::invoke(projection, b);
        if (key_a < key_b)
            return -1;
        if (key_b < key_a)
            return 1;
        return 0;
    }
}
} // namespace cc::impl

/// Orders by each projection in turn, falling through to the next only on a tie.
/// Built by cc::compare_by.
///
/// Every projection goes through cc::invoke, so a pointer-to-member is one.
/// Each is evaluated on every comparison, i.e. O(n log n) times — an expensive one wants cc::sort_by_cached_key.
template <class... ProjFs>
struct cc::lexicographic_comparator
{
    cc::tuple<ProjFs...> projections;

    template <class A, class B>
    [[nodiscard]] constexpr bool operator()(A const& a, B const& b) const
    {
        int verdict = 0;
        // the fold short-circuits on the first projection that separates them, so later ones are never evaluated
        cc::apply([&](auto const&... p) { (void)(((verdict = impl::compare_projection(p, a, b)) != 0) || ...); },
                  projections);
        return verdict < 0;
    }
};

namespace cc
{
/// Builds a lexicographic comparator from projections: order by the first, break ties with the second, and so on.
///
///   cc::sort(entries, cc::compare_by(&entry::group, &entry::name));
///   cc::sort(entries, cc::compare_by(&entry::group, cc::descending(&entry::score)));
///   cc::sort(entries, cc::compare_by([](auto const& e) { return e.name.size(); }, &entry::name));
///
/// The projections are stored by value, so a lambda capturing by reference must outlive the comparator.
template <class... ProjFs>
[[nodiscard]] constexpr auto compare_by(ProjFs&&... projections)
{
    static_assert(sizeof...(projections) >= 1, "cc::compare_by needs at least one projection");
    return cc::lexicographic_comparator<std::remove_cvref_t<ProjFs>...>{
        .projections = cc::tuple<std::remove_cvref_t<ProjFs>...>(cc::forward<ProjFs>(projections)...)};
}

/// Marks a projection as ordering in reverse, for use inside cc::compare_by.
/// On its own it is not a comparator — cc::default_greater is the whole-value spelling of the same idea.
template <class ProjF>
[[nodiscard]] constexpr auto descending(ProjF&& projection)
{
    return cc::descending_projection<std::remove_cvref_t<ProjF>>{.projection = cc::forward<ProjF>(projection)};
}
} // namespace cc
