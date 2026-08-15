#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>

// Input shapes and checkers shared by the sort and selection tests.

namespace sort_test
{
// Vocabulary types (i32/isize/...) available bare inside sort_test, not leaked globally.
using namespace cc::primitive_defines;

enum class pattern;
struct adversary;
struct copy_counted;
} // namespace sort_test

/// The input shapes a comparison sort behaves differently on.
/// Everything except `random` is a case some sort somewhere degenerates to quadratic on.
enum class sort_test::pattern
{
    random,
    sorted,
    reverse,
    mostly_sorted,
    all_equal,
    two_values,
    few_values,
    organ_pipe,
    sawtooth,
    push_front,
    push_middle,
};

/// McIlroy's "antiqsort" adversary: it decides the values as the sort asks about them, always answering so that
/// the pivot the sort is about to pick turns out to be a bad one.
///
/// Every answer stays consistent with the ones before, so this is a valid strict weak ordering throughout —
/// it just makes the median-of-three pivot useless, which is what forces the fallback to engage.
/// The elements are indices into `values`, so the sort may permute them freely.
struct sort_test::adversary
{
    cc::vector<isize> values;
    isize gas = 0;
    isize solid_count = 0;
    isize candidate = 0;
    isize comparison_count = 0;

    static adversary create_for(isize n)
    {
        auto a = adversary{};
        a.gas = n;
        a.values = cc::vector<isize>::create_filled(n, n);
        return a;
    }

    bool operator()(isize x, isize y)
    {
        ++comparison_count;

        if (values[x] == gas && values[y] == gas)
        {
            if (x == candidate)
                values[x] = solid_count++;
            else
                values[y] = solid_count++;
        }

        if (values[x] == gas)
            candidate = x;
        else if (values[y] == gas)
            candidate = y;

        return values[x] < values[y];
    }
};

/// Counts how often it was copied, so a test can pin that the algorithm only ever swaps.
struct sort_test::copy_counted
{
    i32 value = 0;
    static inline isize copies = 0;

    copy_counted() = default;
    explicit copy_counted(i32 v) : value(v) {}

    copy_counted(copy_counted const& rhs) : value(rhs.value) { ++copies; }
    copy_counted& operator=(copy_counted const& rhs)
    {
        value = rhs.value;
        ++copies;
        return *this;
    }
    copy_counted(copy_counted&&) = default;
    copy_counted& operator=(copy_counted&&) = default;
    ~copy_counted() = default;

    friend bool operator<(copy_counted const& a, copy_counted const& b) { return a.value < b.value; }
};

namespace sort_test
{
inline constexpr cc::fixed_array<pattern, 11> all_patterns = {
    pattern::random,    pattern::sorted,     pattern::reverse,     pattern::mostly_sorted,
    pattern::all_equal, pattern::two_values, pattern::few_values,  pattern::organ_pipe,
    pattern::sawtooth,  pattern::push_front, pattern::push_middle,
};

/// Sizes straddling both thresholds the algorithm switches on (insertion sort at 16, ninther at 128).
inline constexpr cc::fixed_array<isize, 14> all_sizes = {0, 1, 2, 3, 5, 15, 16, 17, 40, 127, 128, 129, 500, 1000};

inline char const* to_name(pattern p)
{
    switch (p)
    {
    case pattern::random:
        return "random";
    case pattern::sorted:
        return "sorted";
    case pattern::reverse:
        return "reverse";
    case pattern::mostly_sorted:
        return "mostly_sorted";
    case pattern::all_equal:
        return "all_equal";
    case pattern::two_values:
        return "two_values";
    case pattern::few_values:
        return "few_values";
    case pattern::organ_pipe:
        return "organ_pipe";
    case pattern::sawtooth:
        return "sawtooth";
    case pattern::push_front:
        return "push_front";
    case pattern::push_middle:
        return "push_middle";
    }
    return "?";
}

inline cc::vector<i32> make_pattern(pattern p, isize n, cc::random& rng)
{
    auto values = cc::vector<i32>::create_defaulted(n);

    switch (p)
    {
    case pattern::random:
        for (isize i = 0; i < n; ++i)
            values[i] = rng.uniform(-1000000, 1000000);
        break;

    case pattern::sorted:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i);
        break;

    case pattern::reverse:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(n - i);
        break;

    case pattern::mostly_sorted:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i);
        for (isize k = 0; k < n / 50; ++k)
        {
            auto const a = rng.uniform(isize(0), n - 1);
            auto const b = rng.uniform(isize(0), n - 1);
            cc::swap(values[a], values[b]);
        }
        break;

    case pattern::all_equal:
        for (isize i = 0; i < n; ++i)
            values[i] = 42;
        break;

    case pattern::two_values:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i % 2);
        break;

    case pattern::few_values:
        for (isize i = 0; i < n; ++i)
            values[i] = rng.uniform(0, 3);
        break;

    case pattern::organ_pipe:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i < n / 2 ? i : n - i);
        break;

    case pattern::sawtooth:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i % 32);
        break;

    // sorted but for one element that belongs at the very front — the shape a naive pivot choice hates
    case pattern::push_front:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i);
        if (n > 0)
            values[n - 1] = -1;
        break;

    case pattern::push_middle:
        for (isize i = 0; i < n; ++i)
            values[i] = i32(i);
        if (n > 0)
            values[n / 2] = -1;
        break;
    }

    return values;
}

/// True when both ranges hold the same elements with the same multiplicities.
/// Deliberately counted through a hash map rather than by sorting both — the thing under test cannot verify itself.
template <class RangeT>
[[nodiscard]] inline bool is_permutation_of(RangeT const& a, RangeT const& b)
{
    if (isize(a.size()) != isize(b.size()))
        return false;

    cc::map<i32, isize> counts;
    for (isize i = 0; i < isize(a.size()); ++i)
        counts[a[i]] += 1;
    for (isize i = 0; i < isize(b.size()); ++i)
        counts[b[i]] -= 1;

    for (auto const [key, count] : counts)
        if (count != 0)
            return false;
    return true;
}
} // namespace sort_test
