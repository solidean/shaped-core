#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

#include <list>
#include <vector>

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

TEST("sequence - repeat, single, empty_sequence")
{
    CHECK(equals(cc::repeat(7, 3).to_vector(), {7, 7, 7}));
    CHECK(cc::repeat(7, 0).count() == 0);
    CHECK(cc::repeat(7, 4).sum_from(0) == 28);

    // forever until something cuts it
    static_assert(decltype(cc::repeat(1))::is_infinite);
    static_assert(decltype(cc::repeat(1))::is_non_empty);
    CHECK(cc::repeat(2).take(5).sum_from(0) == 10);
    CHECK(cc::repeat(2).take(5).count() == 5); // take over an infinite source knows its size
    CHECK(cc::repeat(2).any([](int x) { return x == 2; }));

    static_assert(decltype(cc::single(5))::is_non_empty);
    CHECK(cc::single(5).sum() == 5);
    CHECK(cc::single(5).chain(cc::repeat(1, 2)).sum() == 7);

    CHECK(cc::empty_sequence<int>().count() == 0);
    CHECK(!cc::empty_sequence<int>().try_max().has_value());
}

TEST("sequence - generate and iterate are lazy and infinite")
{
    int calls = 0;
    auto counter = [&]
    {
        ++calls;
        return calls;
    };
    CHECK(equals(cc::generate(counter).take(3).to_vector(), {1, 2, 3}));
    CHECK(calls == 3);

    // iterate never computes ahead of the element it makes
    int steps = 0;
    auto const doubling = [&](int x)
    {
        ++steps;
        return x * 2;
    };
    CHECK(equals(cc::iterate(1, doubling).take(5).to_vector(), {1, 2, 4, 8, 16}));
    CHECK(steps == 4);

    CHECK(cc::iterate(1, [](int x) { return x * 3; }).index_of([](int x) { return x > 100; }).value() == 5);
    CHECK(cc::iterate(isize(1), [](isize x) { return x + 1; }).take_while([](isize x) { return x <= 4; }).sum_from(isize(0))
          == 10);

    // an infinite sequence's reductions that would never end static-assert; the trait is what propagates
    static_assert(!decltype(cc::repeat(1).take(3))::is_infinite);

    // and never runs out, so it is non-empty, filtered or not
    static_assert(
        decltype(cc::iterate(1, [](int x) { return x + 1; }).skip_while([](int x) { return x < 5; }))::is_non_empty);
    CHECK(cc::iterate(1, [](int x) { return x + 1; }).filter([](int x) { return x % 7 == 0; }).first() == 7);
    static_assert(decltype(cc::repeat(1).map([](int x) { return x; }))::is_infinite);
    static_assert(decltype(cc::repeat(1).chain(cc::repeat(2, 1)))::is_infinite);
    static_assert(!decltype(cc::zip(cc::repeat(1), cc::repeat(2, 3)))::is_infinite);
}

TEST("sequence - values_of and indices_of over foreign ranges")
{
    // contiguous std storage is a span
    auto sv = std::vector<int>{1, 2, 3, 4};
    static_assert(decltype(cc::values_of(sv))::has_borrowed_elements);
    CHECK(cc::values_of(sv).sum_from(0) == 10);
    CHECK(cc::values_of(sv).reversed().assert_non_empty().first() == 4);
    cc::values_of(sv).each([](int& x) { x *= 10; });
    CHECK(sv[3] == 40);

    // a temporary is owned
    CHECK(cc::values_of(std::vector<int>{5, 6}).sum_from(0) == 11);

    // a C array is borrowed, and indices_of knows its extent
    int arr[] = {9, 8, 7};
    static_assert(decltype(cc::values_of(arr))::is_non_empty);
    static_assert(decltype(cc::indices_of(arr))::is_non_empty);
    CHECK(cc::values_of(arr).min() == 7);
    CHECK(cc::indices_of(arr).count() == 3);
    CHECK(cc::indices_of(sv).contains(3));
    CHECK(!cc::indices_of(sv).contains(4));

    // anything else walks begin/end
    auto list = std::list<int>{3, 1, 2};
    CHECK(cc::values_of(list).sum_from(0) == 6);
    CHECK(cc::values_of(list).find([](int x) { return x == 1; }) == &*std::next(list.begin()));
    int seen = 0;
    for (int x : cc::values_of(list))
        seen = seen * 10 + x;
    CHECK(seen == 312);

    // a temporary list is owned through one allocation, so assert_non_empty may move it after pulling an element
    CHECK(cc::values_of(std::list<int>{4, 5}).filter([](int x) { return x > 0; }).assert_non_empty().max() == 5);

    // an initializer list is borrowed for the full expression
    CHECK(cc::values_of({1, 2, 3}).sum_from(0) == 6);

    // a cc container passes straight through to its own values()
    auto const cv = cc::vector<int>{1, 2};
    static_assert(std::is_same_v<decltype(cc::values_of(cv)), decltype(cv.values())>);

    // and foreign ranges zip and chain with cc ones
    CHECK(cc::zip(sv, cv).map([](int a, int b) { return a * b; }).sum_from(0) == 10 + 40);
    CHECK(cv.values().chain(arr).count() == 5);
}

TEST("sequence - next and next_back may interleave on a source")
{
    int arr[] = {1, 2, 3, 4, 5};
    auto s = cc::impl::span_source<int>(arr, arr + 5);
    CHECK(*s.next() == 1);
    CHECK(*s.next_back() == 5);
    CHECK(*s.next() == 2);
    CHECK(*s.next_back() == 4);
    CHECK(*s.next() == 3);
    CHECK(s.next() == nullptr);
    CHECK(s.next_back() == nullptr);

    auto c = cc::impl::chain_source<cc::impl::span_source<int>, cc::impl::span_source<int>>(
        cc::impl::span_source<int>(arr, arr + 2), cc::impl::span_source<int>(arr + 2, arr + 3));
    CHECK(*c.next_back() == 3);
    CHECK(*c.next_back() == 2);
    CHECK(*c.next() == 1);
    CHECK(c.next() == nullptr);
}
