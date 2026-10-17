#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::vector<int> make_numbers()
{
    return {3, 1, 4, 1, 5, 9, 2, 6};
}

template <class T>
bool equals(cc::vector<T> const& a, std::initializer_list<T> b)
{
    if (a.size() != isize(b.size()))
        return false;
    isize i = 0;
    for (auto const& x : b)
        if (!(a[i++] == x))
            return false;
    return true;
}
} // namespace

TEST("sequence - take and skip")
{
    auto const v = make_numbers();

    CHECK(equals(v.values().take(3).to_vector(), {3, 1, 4}));
    CHECK(equals(v.values().skip(5).to_vector(), {9, 2, 6}));
    CHECK(equals(v.values().skip(2).take(2).to_vector(), {4, 1}));
    CHECK(v.values().take(100).count() == 8);
    CHECK(v.values().skip(100).count() == 0);
    CHECK(v.values().take(0).count() == 0);
    CHECK(v.values().take(-1).count() == 0);

    // a contiguous source truncates and advances itself, so take and skip add no layer
    static_assert(std::is_same_v<decltype(v.values().take(3)), decltype(v.values())>);
    static_assert(std::is_same_v<decltype(v.values().skip(3)), decltype(v.values())>);

    // over a filter they are adaptors, and the filter still sees every element it passes over
    auto const odd = [](int x) { return x % 2 == 1; };
    CHECK(equals(v.values().filter(odd).take(3).to_vector(), {3, 1, 1}));
    CHECK(equals(v.values().filter(odd).skip(3).to_vector(), {5, 9}));
    CHECK(v.values().filter(odd).take(2).count() == 2);
}

TEST("sequence - take_while and skip_while")
{
    auto const v = make_numbers();

    CHECK(equals(v.values().take_while([](int x) { return x < 5; }).to_vector(), {3, 1, 4, 1}));
    CHECK(equals(v.values().skip_while([](int x) { return x < 5; }).to_vector(), {5, 9, 2, 6}));
    CHECK(v.values().take_while([](int) { return false; }).count() == 0);
    CHECK(v.values().skip_while([](int) { return true; }).count() == 0);

    // the predicate's index is the position in the input
    CHECK(equals(v.values().take_while([](isize i, int) { return i < 2; }).to_vector(), {3, 1}));

    // range-for goes through next(), the external path
    int total = 0;
    for (int x : v.values().skip_while([](int x) { return x != 9; }))
        total += x;
    CHECK(total == 17);
}

TEST("sequence - chain")
{
    auto a = cc::vector<int>{1, 2};
    auto const b = cc::vector<int>{3, 4, 5};

    // items must match exactly, so a mutable and a const vector chain through their const views
    auto const ca = cc::vector<int>{1, 2};
    CHECK(equals(ca.values().chain(b).to_vector(), {1, 2, 3, 4, 5}));
    CHECK(equals(a.values().chain(cc::vector<int>{7}).map([](int x) { return x; }).to_vector(), {1, 2, 7}));
    CHECK(ca.values().chain(b.values()).count() == 5);
    CHECK(ca.values().chain(b).assert_non_empty().max() == 5);

    // a non-empty side makes the chain non-empty
    static_assert(decltype(ca.values().assert_non_empty().chain(b))::is_non_empty);
    static_assert(!decltype(ca.values().chain(b))::is_non_empty);

    // contains forwards to both sides where both know membership
    CHECK(cc::iota(0, 3).chain(cc::iota(10, 13)).contains(11));
    CHECK(!cc::iota(0, 3).chain(cc::iota(10, 13)).contains(5));

    int seen = 0;
    for (int x : ca.values().chain(b))
        seen += x;
    CHECK(seen == 15);
}

TEST("sequence - zip spreads its items into callbacks")
{
    auto const names = cc::vector<int>{10, 20, 30};
    auto const weights = cc::vector<float>{0.5f, 1.0f, 2.0f, 4.0f};

    // the shortest side ends the zip
    CHECK(names.values().zip(weights).count() == 3);
    CHECK(cc::zip(names, weights).map([](int n, float w) { return float(n) * w; }).sum_from(0.0f) == 85.0f);
    CHECK(cc::zip(names, weights).any([](int n, float w) { return n == 20 && w == 1.0f; }));
    CHECK(cc::zip(names, weights).index_of([](int, float w) { return w > 1.5f; }).value() == 2);

    // the index still comes first
    isize last = -1;
    cc::zip(names, weights).each([&](isize i, int, float) { last = i; });
    CHECK(last == 2);

    // a zip materializes as tuples
    auto const pairs = cc::zip(names, weights).to_vector();
    static_assert(std::is_same_v<decltype(pairs), cc::vector<cc::tuple<int, float>> const>);
    CHECK(pairs.size() == 3);
    CHECK(pairs[1].get<0>() == 20);
    CHECK(pairs[1].get<1>() == 1.0f);

    // three sides, and items written through
    auto a = cc::vector<int>{1, 2, 3};
    auto b = cc::vector<int>{10, 20, 30};
    auto out = cc::vector<int>{0, 0, 0};
    cc::zip(a, b, out).each([](int x, int y, int& o) { o = x + y; });
    CHECK(equals(out, {11, 22, 33}));

    // range-for binds the pack
    int dot = 0;
    for (auto&& [x, y] : cc::zip(a, b))
        dot += x * y;
    CHECK(dot == 140);

    // zip over a filter takes the external path for the second side
    CHECK(equals(
        a.values().zip(b.values().filter([](int y) { return y != 20; })).map([](int x, int y) { return x + y; }).to_vector(),
        {11, 32}));
}

TEST("sequence - enumerate")
{
    auto const v = cc::vector<int>{5, 6, 7};

    CHECK(v.values().enumerate().map([](isize i, int x) { return int(i) * x; }).sum_from(0) == 0 + 6 + 14);
    CHECK(v.values().enumerate().filter([](isize i, int) { return i != 1; }).map([](isize, int x) { return x; }).sum_from(0)
          == 12);

    isize idx_sum = 0;
    for (auto&& [i, x] : v.values().enumerate())
        idx_sum += i;
    CHECK(idx_sum == 3);
}

TEST("sequence - flatten and flat_map")
{
    auto const nested = cc::vector<cc::vector<int>>{{1, 2}, {}, {3}, {4, 5, 6}};

    CHECK(equals(nested.values().flatten().to_vector(), {1, 2, 3, 4, 5, 6}));
    CHECK(nested.values().flatten().sum_from(0) == 21);
    CHECK(nested.values().flatten().count() == 6);
    CHECK(nested.values().flatten().find([](int x) { return x == 3; }) == &nested[2][0]);

    // f may return a container by value; each one is owned while it is walked
    CHECK(equals(cc::iota(1, 4).flat_map([](isize n) { return cc::vector<isize>::create_filled(n, n); }).to_vector(),
                 std::initializer_list<isize>{1, 2, 2, 3, 3, 3}));

    // or a sequence
    CHECK(cc::iota(0, 3).flat_map([](isize n) { return cc::iota(0, n); }).count() == 3);

    // external iteration across inner boundaries, empty inners included
    int seen = 0;
    for (int x : nested.values().flatten())
        seen = seen * 10 + x;
    CHECK(seen == 123456);
}

TEST("sequence - consume runs every side effect")
{
    auto const v = make_numbers();

    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x;
    };

    // count over a sized map knows the answer without calling the map
    CHECK(v.values().map(counted).count() == 8);
    CHECK(calls == 0);

    v.values().map(counted).consume();
    CHECK(calls == 8);
}

TEST("sequence - average, projections, by-key and ends")
{
    auto const v = make_numbers();

    CHECK(v.values().assert_non_empty().average() == 31.0 / 8.0);
    static_assert(std::is_same_v<decltype(v.values().assert_non_empty().average()), f64>);
    auto const f = cc::vector<float>{1.0f, 2.0f};
    static_assert(std::is_same_v<decltype(f.values().assert_non_empty().average()), float>);
    CHECK(f.values().assert_non_empty().average() == 1.5f);

    struct particle
    {
        float mass;
        int id;
    };
    auto const ps = cc::vector<particle>{{2.0f, 7}, {0.5f, 3}, {4.0f, 1}};
    CHECK(ps.values().assert_non_empty().sum(&particle::mass) == 6.5f);
    CHECK(ps.values().assert_non_empty().max(&particle::id) == 7);
    CHECK(ps.values().assert_non_empty().min_by(&particle::mass).id == 3);
    CHECK(ps.values().assert_non_empty().max_by([](particle const& p) { return p.mass; }).id == 1);
    CHECK(ps.values().assert_non_empty().average(&particle::mass) == 6.5f / 3.0f);

    CHECK(v.values().assert_non_empty().first() == 3);
    CHECK(v.values().assert_non_empty().last() == 6);
    CHECK(v.values().first_or(-1) == 3);
    CHECK(v.values().try_last().value() == 6);
    CHECK(v.values().filter([](int x) { return x > 4; }).try_last().value() == 6);
    CHECK(v.values().filter([](int x) { return x > 4; }).assert_non_empty().last() == 6);
    CHECK(!v.values().filter([](int x) { return x > 100; }).try_first().has_value());
    CHECK(cc::vector<int>().values().first_or(-1) == -1);
}

TEST("sequence - iota")
{
    CHECK(cc::iota(5).count() == 5);
    CHECK(cc::iota(2, 6).sum_from(isize(0)) == 14);
    CHECK(cc::iota(10).contains(9));
    CHECK(!cc::iota(10).contains(10));
    CHECK(equals(cc::iota(10).filter([](isize i) { return i % 3 == 0; }).to_vector(),
                 std::initializer_list<isize>{0, 3, 6, 9}));
}
