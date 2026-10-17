#include <clean-core/common/macros.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// Codegen probes: each sequence spelling beside the hand-written loop it must match.
// `uv run dev.py assembly show seq_probe_<name>` on a release preset compares a pair; the test only keeps them referenced.
// A sequence use that compiles to worse code than its naive loop is a regression, beyond inherent noise.

namespace
{
CC_DONT_INLINE int seq_probe_sum_naive(cc::span<int const> s)
{
    int acc = s[0];
    for (isize i = 1; i < s.size(); ++i)
        acc += s[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_sum_seq(cc::span<int const> s)
{
    return s.values().assert_non_empty().sum();
}

CC_DONT_INLINE int seq_probe_sum_from_naive(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s)
        acc += x;
    return acc;
}
CC_DONT_INLINE int seq_probe_sum_from_seq(cc::span<int const> s)
{
    return s.values().sum_from(0);
}

CC_DONT_INLINE isize seq_probe_count_if_naive(cc::span<int const> s)
{
    isize n = 0;
    for (int x : s)
        if (x > 0)
            ++n;
    return n;
}
CC_DONT_INLINE isize seq_probe_count_if_seq(cc::span<int const> s)
{
    return s.values().count_if([](int x) { return x > 0; });
}

CC_DONT_INLINE isize seq_probe_filter_count_seq(cc::span<int const> s)
{
    return s.values().filter([](int x) { return x > 0; }).count();
}

CC_DONT_INLINE bool seq_probe_any_naive(cc::span<int const> s, int v)
{
    for (int x : s)
        if (x == v)
            return true;
    return false;
}
CC_DONT_INLINE bool seq_probe_any_seq(cc::span<int const> s, int v)
{
    return s.values().any([v](int x) { return x == v; });
}

CC_DONT_INLINE int seq_probe_min_naive(cc::span<int const> s)
{
    int m = s[0];
    for (isize i = 1; i < s.size(); ++i)
        if (s[i] < m)
            m = s[i];
    return m;
}
CC_DONT_INLINE int seq_probe_min_seq(cc::span<int const> s)
{
    return s.values().assert_non_empty().min();
}

CC_DONT_INLINE int seq_probe_chain_naive(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s)
    {
        auto const y = x * 3;
        if (y > 4)
            acc += y;
    }
    return acc;
}
CC_DONT_INLINE int seq_probe_chain_seq(cc::span<int const> s)
{
    return s.values().map([](int x) { return x * 3; }).filter([](int y) { return y > 4; }).sum_from(0);
}

CC_DONT_INLINE int seq_probe_range_for_seq(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s.values().filter([](int x) { return x > 0; }))
        acc += x;
    return acc;
}
CC_DONT_INLINE int seq_probe_range_for_each_seq(cc::span<int const> s)
{
    int acc = 0;
    s.values().filter([](int x) { return x > 0; }).each([&](int x) { acc += x; });
    return acc;
}
CC_DONT_INLINE int seq_probe_range_for_naive(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s)
        if (x > 0)
            acc += x;
    return acc;
}

CC_DONT_INLINE isize seq_probe_index_of_naive(cc::span<int const> s, int v)
{
    for (isize i = 0; i < s.size(); ++i)
        if (s[i] == v)
            return i;
    return -1;
}
CC_DONT_INLINE isize seq_probe_index_of_seq(cc::span<int const> s, int v)
{
    return s.values().index_of([v](int x) { return x == v; }).value_or(-1);
}

CC_DONT_INLINE int seq_probe_plain_for_seq(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s.values())
        acc += x;
    return acc;
}

CC_DONT_INLINE int seq_probe_map_for_naive(cc::span<int const> s)
{
    int acc = 0;
    for (int x : s)
        acc += x * 3;
    return acc;
}
CC_DONT_INLINE int seq_probe_map_for_seq(cc::span<int const> s)
{
    int acc = 0;
    for (int y : s.values().map([](int x) { return x * 3; }))
        acc += y;
    return acc;
}

CC_DONT_INLINE isize seq_probe_indexed_naive(cc::span<int const> s)
{
    isize acc = 0;
    for (isize i = 0; i < s.size(); ++i)
        acc += i * s[i];
    return acc;
}
CC_DONT_INLINE isize seq_probe_indexed_seq(cc::span<int const> s)
{
    isize acc = 0;
    s.values().each([&](isize i, int x) { acc += i * x; });
    return acc;
}

CC_DONT_INLINE bool seq_probe_indices_contains_seq(cc::vector<int> const& v, isize i)
{
    return v.indices().contains(i);
}
} // namespace

TEST("sequence - codegen probes agree with their naive loops")
{
    auto const v = cc::vector<int>{3, -1, 4, -1, 5, 9, -2, 6, 5, 3, 5};
    auto const s = cc::span<int const>(v);

    CHECK(seq_probe_sum_seq(s) == seq_probe_sum_naive(s));
    CHECK(seq_probe_sum_from_seq(s) == seq_probe_sum_from_naive(s));
    CHECK(seq_probe_count_if_seq(s) == seq_probe_count_if_naive(s));
    CHECK(seq_probe_filter_count_seq(s) == seq_probe_count_if_naive(s));
    CHECK(seq_probe_any_seq(s, 9) == seq_probe_any_naive(s, 9));
    CHECK(seq_probe_any_seq(s, 7) == seq_probe_any_naive(s, 7));
    CHECK(seq_probe_min_seq(s) == seq_probe_min_naive(s));
    CHECK(seq_probe_chain_seq(s) == seq_probe_chain_naive(s));
    CHECK(seq_probe_range_for_seq(s) == seq_probe_range_for_naive(s));
    CHECK(seq_probe_range_for_each_seq(s) == seq_probe_range_for_naive(s));
    CHECK(seq_probe_index_of_seq(s, 9) == seq_probe_index_of_naive(s, 9));
    CHECK(seq_probe_index_of_seq(s, 7) == seq_probe_index_of_naive(s, 7));
    CHECK(seq_probe_plain_for_seq(s) == seq_probe_sum_from_naive(s));
    CHECK(seq_probe_indexed_seq(s) == seq_probe_indexed_naive(s));
    CHECK(seq_probe_map_for_seq(s) == seq_probe_map_for_naive(s));
    CHECK(seq_probe_indices_contains_seq(v, 10));
    CHECK(!seq_probe_indices_contains_seq(v, 11));
}
