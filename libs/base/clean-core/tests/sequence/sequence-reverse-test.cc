#include <clean-core/container/map.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
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

TEST("sequence - reversed over contiguous sources")
{
    auto v = cc::vector<int>{1, 2, 3, 4, 5};

    CHECK(equals(v.values().reversed().to_vector(), {5, 4, 3, 2, 1}));
    CHECK(equals(cc::iota(4).reversed().to_vector(), std::initializer_list<isize>{3, 2, 1, 0}));
    CHECK(equals(cc::vector<int>{7, 8}.values().reversed().to_vector(), {8, 7}));

    // positional capabilities mirror, so take and skip on a reversed span stay pointer ranges
    static_assert(std::is_same_v<decltype(v.values().reversed().take(2)), decltype(v.values().reversed())>);
    CHECK(equals(v.values().reversed().take(2).to_vector(), {5, 4}));
    CHECK(equals(v.values().reversed().skip(3).to_vector(), {2, 1}));
    CHECK(v.values().reversed().assert_non_empty().first() == 5);

    // twice is the original order
    CHECK(equals(v.values().reversed().reversed().to_vector(), {1, 2, 3, 4, 5}));

    // external iteration from the back
    int seen = 0;
    for (int x : v.values().reversed())
        seen = seen * 10 + x;
    CHECK(seen == 54321);

    // references survive, so a reversed walk writes through
    v.values().reversed().take(1).each([](int& x) { x = 50; });
    CHECK(v[4] == 50);
}

TEST("sequence - reversed through the adaptors")
{
    auto const v = cc::vector<int>{1, 2, 3, 4, 5, 6};
    auto const twice = [](int x) { return x * 2; };
    auto const even = [](int x) { return x % 2 == 0; };

    CHECK(equals(v.values().map(twice).reversed().to_vector(), {12, 10, 8, 6, 4, 2}));
    CHECK(equals(v.values().filter(even).reversed().to_vector(), {6, 4, 2}));
    CHECK(equals(v.values().take(4).filter(even).reversed().to_vector(), {4, 2}));
    // take and skip need a sized input to know where they end from the back, so over a filter they do not reverse
    static_assert(!decltype(v.values().filter(even).take(2))::is_reversible);
    static_assert(!decltype(v.values().filter(even).skip(1))::is_reversible);
    CHECK(equals(v.values().map(twice).take(2).reversed().to_vector(), {4, 2}));

    auto const a = cc::vector<int>{1, 2};
    auto const b = cc::vector<int>{3, 4};
    CHECK(equals(a.values().chain(b).reversed().to_vector(), {4, 3, 2, 1}));

    // a zip from the back first drops what reaches past the shortest side
    auto const longer = cc::vector<int>{10, 20, 30, 40};
    auto const pairs = cc::zip(a, longer).reversed().map([](int x, int y) { return x + y; }).to_vector();
    CHECK(equals(pairs, {22, 11}));

    // a callback's index stays the position from the front
    CHECK(equals(v.values().map([](isize i, int x) { return int(i) * 100 + x; }).reversed().take(2).to_vector(),
                 {506, 405}));
    CHECK(equals(v.values().filter([](isize i, int) { return i < 2; }).reversed().to_vector(), {2, 1}));

    // a ringbuffer's two segments chain, so it reverses too
    auto rb = cc::ringbuffer<int>();
    for (int i = 0; i < 3; ++i)
        rb.push_back(i);
    rb.push_front(-1);
    CHECK(equals(rb.values().reversed().to_vector(), {2, 1, 0, -1}));

    // a map's iteration order is arbitrary, and its chains are singly linked, so it is not reversible
    static_assert(!decltype(cc::map<int, int>().keys())::is_reversible);
    static_assert(!decltype(v.values().take_while(even))::is_reversible);
}

TEST("sequence - last, find_last and last_index_of")
{
    auto v = cc::vector<int>{3, 1, 4, 1, 5, 9, 2, 6};

    CHECK(v.values().assert_non_empty().last() == 6);
    CHECK(v.values().filter([](int x) { return x < 5; }).try_last().value() == 2);
    CHECK(v.values().map([](int x) { return x + 1; }).try_last().value() == 7);
    CHECK(!cc::vector<int>().values().try_last().has_value());

    CHECK(v.values().find_last([](int x) { return x == 1; }) == &v[3]);
    CHECK(v.values().find_last([](int x) { return x == 7; }) == nullptr);
    CHECK(v.values().find_last([](isize i, int) { return i < 2; }) == &v[1]);

    CHECK(v.values().last_index_of([](int x) { return x == 1; }).value() == 3);
    CHECK(v.values().last_index_of([](int x) { return x > 4; }).value() == 7);
    CHECK(!v.values().last_index_of([](int x) { return x > 100; }).has_value());
}
