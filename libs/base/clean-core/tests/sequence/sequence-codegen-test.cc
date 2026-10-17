#include <clean-core/common/macros.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

#include <vector>

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

CC_DONT_INLINE int seq_probe_zip_naive(cc::span<int const> a, cc::span<int const> b)
{
    int acc = 0;
    auto const n = a.size() < b.size() ? a.size() : b.size();
    for (isize i = 0; i < n; ++i)
        acc += a[i] * b[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_zip_seq(cc::span<int const> a, cc::span<int const> b)
{
    return cc::zip(a, b).map([](int x, int y) { return x * y; }).sum_from(0);
}

CC_DONT_INLINE int seq_probe_take_naive(cc::span<int const> s, isize n)
{
    int acc = 0;
    auto const k = n < s.size() ? n : s.size();
    for (isize i = 0; i < k; ++i)
        acc += s[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_take_seq(cc::span<int const> s, isize n)
{
    return s.values().take(n).sum_from(0);
}

CC_DONT_INLINE isize seq_probe_enumerate_seq(cc::span<int const> s)
{
    return s.values().enumerate().map([](isize i, int x) { return i * x; }).sum_from(isize(0));
}

CC_DONT_INLINE int seq_probe_chain_two_naive(cc::span<int const> a, cc::span<int const> b)
{
    int acc = 0;
    for (int x : a)
        acc += x;
    for (int x : b)
        acc += x;
    return acc;
}
CC_DONT_INLINE int seq_probe_chain_two_seq(cc::span<int const> a, cc::span<int const> b)
{
    return a.values().chain(b).sum_from(0);
}

CC_DONT_INLINE int seq_probe_flatten_naive(cc::vector<cc::vector<int>> const& vv)
{
    int acc = 0;
    for (auto const& v : vv)
        for (int x : v)
            acc += x;
    return acc;
}
CC_DONT_INLINE int seq_probe_flatten_seq(cc::vector<cc::vector<int>> const& vv)
{
    return vv.values().flatten().sum_from(0);
}

CC_DONT_INLINE float seq_probe_map_values_naive(cc::map<int, float> const& m)
{
    float acc = 0;
    for (auto [k, v] : m)
        acc += v;
    return acc;
}
CC_DONT_INLINE float seq_probe_map_values_seq(cc::map<int, float> const& m)
{
    return m.values().sum_from(0.0f);
}

CC_DONT_INLINE f64 seq_probe_average_naive(cc::span<int const> s)
{
    i64 acc = s[0];
    for (isize i = 1; i < s.size(); ++i)
        acc += s[i];
    return f64(acc) / f64(s.size());
}
CC_DONT_INLINE f64 seq_probe_average_seq(cc::span<int const> s)
{
    return s.values().assert_non_empty().average();
}

CC_DONT_INLINE int seq_probe_reversed_naive(cc::span<int const> s)
{
    int acc = 0;
    for (isize i = s.size() - 1; i >= 0; --i)
        acc = acc * 3 + s[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_reversed_seq(cc::span<int const> s)
{
    int acc = 0;
    s.values().reversed().each([&](int x) { acc = acc * 3 + x; });
    return acc;
}

CC_DONT_INLINE int const* seq_probe_find_last_naive(cc::span<int const> s, int v)
{
    for (isize i = s.size() - 1; i >= 0; --i)
        if (s[i] == v)
            return &s[i];
    return nullptr;
}
CC_DONT_INLINE int const* seq_probe_find_last_seq(cc::span<int const> s, int v)
{
    return s.values().find_last([v](int x) { return x == v; });
}

CC_DONT_INLINE int seq_probe_values_of_naive(std::vector<int> const& v)
{
    int acc = 0;
    for (int x : v)
        acc += x;
    return acc;
}
CC_DONT_INLINE int seq_probe_values_of_seq(std::vector<int> const& v)
{
    return cc::values_of(v).sum_from(0);
}

CC_DONT_INLINE int seq_probe_pairwise_naive(cc::span<int const> s)
{
    int acc = 0;
    for (isize i = 0; i + 1 < s.size(); ++i)
        acc += s[i + 1] - s[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_pairwise_seq(cc::span<int const> s)
{
    return s.values().pairwise().map([](int a, int b) { return b - a; }).sum_from(0);
}

CC_DONT_INLINE int seq_probe_chunks_naive(cc::span<int const> s)
{
    int acc = 0;
    for (isize i = 0; i + 3 <= s.size(); i += 3)
        acc += s[i] * s[i + 1] + s[i + 2];
    return acc;
}
CC_DONT_INLINE int seq_probe_chunks_seq(cc::span<int const> s)
{
    return s.values().chunks<3>().map([](int a, int b, int c) { return a * b + c; }).sum_from(0);
}

CC_DONT_INLINE int seq_probe_step_naive(cc::span<int const> s)
{
    int acc = 0;
    for (isize i = 0; i < s.size(); i += 2)
        acc += s[i];
    return acc;
}
CC_DONT_INLINE int seq_probe_step_seq(cc::span<int const> s)
{
    return s.values().step_by(2).sum_from(0);
}

CC_DONT_INLINE int seq_probe_min_max_naive(cc::span<int const> s)
{
    int lo = s[0];
    int hi = s[0];
    for (isize i = 1; i < s.size(); ++i)
    {
        if (s[i] < lo)
            lo = s[i];
        else if (hi < s[i])
            hi = s[i];
    }
    return hi - lo;
}
CC_DONT_INLINE int seq_probe_min_max_seq(cc::span<int const> s)
{
    auto const [lo, hi] = s.values().assert_non_empty().min_max();
    return hi - lo;
}

CC_DONT_INLINE bool seq_probe_equals_naive(cc::span<int const> a, cc::span<int const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
CC_DONT_INLINE bool seq_probe_equals_seq(cc::span<int const> a, cc::span<int const> b)
{
    return a.values().equals(b);
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
    CHECK(seq_probe_zip_seq(s, s) == seq_probe_zip_naive(s, s));
    CHECK(seq_probe_take_seq(s, 5) == seq_probe_take_naive(s, 5));
    CHECK(seq_probe_enumerate_seq(s) == seq_probe_indexed_naive(s));
    CHECK(seq_probe_chain_two_seq(s, s) == seq_probe_chain_two_naive(s, s));

    auto const vv = cc::vector<cc::vector<int>>{{1, 2, 3}, {}, {4, 5}};
    CHECK(seq_probe_flatten_seq(vv) == seq_probe_flatten_naive(vv));

    auto m = cc::map<int, float>();
    m[1] = 0.5f;
    m[4] = 1.5f;
    CHECK(seq_probe_map_values_seq(m) == seq_probe_map_values_naive(m));

    CHECK(seq_probe_average_seq(s) == seq_probe_average_naive(s));
    CHECK(seq_probe_reversed_seq(s) == seq_probe_reversed_naive(s));
    CHECK(seq_probe_find_last_seq(s, 5) == seq_probe_find_last_naive(s, 5));
    CHECK(seq_probe_find_last_seq(s, 7) == seq_probe_find_last_naive(s, 7));
    auto const sv = std::vector<int>{1, 2, 3, 4, 5};
    CHECK(seq_probe_values_of_seq(sv) == seq_probe_values_of_naive(sv));
    CHECK(seq_probe_pairwise_seq(s) == seq_probe_pairwise_naive(s));
    CHECK(seq_probe_chunks_seq(s) == seq_probe_chunks_naive(s));
    CHECK(seq_probe_step_seq(s) == seq_probe_step_naive(s));
    CHECK(seq_probe_min_max_seq(s) == seq_probe_min_max_naive(s));
    CHECK(seq_probe_equals_seq(s, s) == seq_probe_equals_naive(s, s));
    CHECK(seq_probe_indices_contains_seq(v, 10));
    CHECK(!seq_probe_indices_contains_seq(v, 11));
}
