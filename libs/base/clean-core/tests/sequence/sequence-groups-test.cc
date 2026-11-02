#include <clean-core/container/map.hh>
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

auto const odd = [](int x) { return x % 2 == 1; };
} // namespace

TEST("sequence - pairwise and windows over positions")
{
    auto const v = cc::vector<int>{1, 4, 9, 16, 25};

    // a callback takes the neighbours spread
    CHECK(equals(v.values().pairwise().map([](int a, int b) { return b - a; }).to_vector(), {3, 5, 7, 9}));
    static_assert(decltype(v.values().pairwise())::has_known_size);
    CHECK(v.values().pairwise().count() == 4);
    CHECK(v.values().windows<3>().count() == 3);
    CHECK(equals(v.values().windows<3>().map([](int a, int b, int c) { return a + b + c; }).to_vector(), {14, 29, 50}));

    // fewer elements than the window gives none
    CHECK(cc::vector<int>{1}.values().pairwise().count() == 0);
    CHECK(cc::vector<int>().values().windows<3>().count() == 0);

    // the window references the elements, so it can write through them
    auto w = cc::vector<int>{1, 2, 3};
    w.values().pairwise().each([](int& a, int& b) { b += a; });
    CHECK(equals(w, {1, 3, 6}));

    // positions make it reversible and indexable
    CHECK(equals(v.values().pairwise().reversed().map([](int a, int b) { return a + b; }).to_vector(), {41, 25, 13, 5}));
    CHECK(v.values().pairwise().assert_non_empty().last().get<0>() == 16);

    int pairs = 0;
    for (auto&& [a, b] : v.values().pairwise())
        pairs += b > a ? 1 : 0;
    CHECK(pairs == 4);
}

TEST("sequence - windows over a source without positions buffer the last N")
{
    auto const v = cc::vector<int>{1, 3, 4, 5, 7, 9};

    // a filter has no positions, so the window keeps pointers to the last N references
    static_assert(!decltype(v.values().filter(odd).pairwise())::has_known_size);
    CHECK(equals(v.values().filter(odd).pairwise().map([](int a, int b) { return a * 10 + b; }).to_vector(),
                 {13, 35, 57, 79}));
    CHECK(v.values().filter(odd).windows<3>().count() == 3);

    // a prvalue input is buffered by value
    auto s = cc::iota(1, 6).filter([](isize) { return true; }).map([](isize i) { return i * i; });
    static_assert(std::is_same_v<decltype(s)::item_t, isize>);
    CHECK(equals(cc::move(s).pairwise().map([](isize a, isize b) { return b - a; }).to_vector(),
                 std::initializer_list<isize>{3, 5, 7, 9}));

    // through next() as well
    int seen = 0;
    for (auto&& [a, b] : v.values().filter(odd).pairwise())
        seen = seen * 10 + (b - a);
    CHECK(seen == 2222);
}

TEST("sequence - chunks")
{
    auto const idx = cc::vector<int>{0, 1, 2, 2, 1, 3, 4};

    // a triangle index buffer, the trailing partial group dropped
    CHECK(idx.values().chunks<3>().count() == 2);
    CHECK(equals(idx.values().chunks<3>().map([](int a, int b, int c) { return a + b + c; }).to_vector(), {3, 6}));
    CHECK(equals(idx.values().chunks<3>().reversed().map([](int a, int, int) { return a; }).to_vector(), {2, 0}));

    // without positions
    CHECK(equals(
        idx.values().filter([](int) { return true; }).chunks<2>().map([](int a, int b) { return a * 10 + b; }).to_vector(),
        {1, 22, 13}));

    // a chunk materializes as a tuple
    auto const tris = idx.values().chunks<3>().to_vector();
    CHECK(tris[1].get<2>() == 3);
}

TEST("sequence - step_by")
{
    auto const v = cc::vector<int>{0, 1, 2, 3, 4, 5, 6};

    CHECK(equals(v.values().step_by(3).to_vector(), {0, 3, 6}));
    CHECK(v.values().step_by(3).count() == 3);
    CHECK(v.values().step_by(2).count() == 4);
    CHECK(equals(v.values().step_by(1).to_vector(), {0, 1, 2, 3, 4, 5, 6}));
    CHECK(equals(v.values().step_by(3).reversed().to_vector(), {6, 3, 0}));
    CHECK(v.values().step_by(4).assert_non_empty().last() == 4);

    // without positions the skipped ones are still evaluated
    CHECK(equals(v.values().filter([](int x) { return x != 1; }).step_by(2).to_vector(), {0, 3, 5}));
    int seen = 0;
    for (int x : v.values().filter([](int) { return true; }).step_by(3))
        seen = seen * 10 + x;
    CHECK(seen == 36);

    // an infinite input stays infinite, and positions are kept over iota
    CHECK(equals(cc::iota(0, 100).step_by(25).to_vector(), std::initializer_list<isize>{0, 25, 50, 75}));
    CHECK(equals(cc::repeat(1).step_by(5).take(2).to_vector(), {1, 1}));
}

TEST("sequence - inspect, filter_map, scan, dedup")
{
    auto const v = cc::vector<int>{1, 1, 2, 3, 3, 3, 1};

    int peeked = 0;
    CHECK(v.values().inspect([&](int x) { peeked += x; }).filter(odd).count() == 6);
    CHECK(peeked == 14);

    // inspect has no size, so count() passes every element by it
    static_assert(!decltype(v.values().inspect([](int) {}))::has_known_size);
    int counted = 0;
    CHECK(v.values().inspect([&](int) { ++counted; }).count() == 7);
    CHECK(counted == 7);

    // an optional result keeps its value, an empty one drops the element
    auto const halve_evens = [](int x) { return x % 2 == 0 ? cc::optional<int>(x / 2) : cc::optional<int>(); };
    CHECK(equals(v.values().filter_map(halve_evens).to_vector(), {1}));
    auto words = cc::map<int, int>();
    words[1] = 10;
    words[3] = 30;
    auto const found = v.values().filter_map([&](int x) { return words.get_ptr(x); }).sum_from(0);
    CHECK(found == 10 + 10 + 30 + 30 + 30 + 10);
    static_assert(decltype(v.values().filter_map([&](int x) { return words.get_ptr(x); }))::has_stable_elements);

    // prefix sums
    CHECK(equals(v.values().scan(0, [](int acc, int x) { return acc + x; }).to_vector(), {1, 2, 4, 7, 10, 13, 14}));
    CHECK(v.values().scan(0, [](int acc, int x) { return acc + x; }).count() == 7);

    // consecutive duplicates only
    CHECK(equals(v.values().dedup().to_vector(), {1, 2, 3, 1}));
    CHECK(equals(cc::iota(10).map([](isize i) { return int(i / 3); }).dedup().to_vector(), {0, 1, 2, 3}));
    CHECK(equals(v.values().dedup_by([](int x) { return x % 2; }).to_vector(), {1, 2, 3}));
}

TEST("sequence - take_last and skip_last")
{
    auto const v = cc::vector<int>{1, 2, 3, 4, 5};

    CHECK(equals(v.values().take_last(2).to_vector(), {4, 5}));
    CHECK(equals(v.values().skip_last(2).to_vector(), {1, 2, 3}));
    CHECK(v.values().take_last(10).count() == 5);
    CHECK(v.values().skip_last(10).count() == 0);
    CHECK(v.values().take_last(0).count() == 0);
    CHECK(equals(v.values().map([](int x) { return x * 2; }).take_last(1).to_vector(), {10}));
}
