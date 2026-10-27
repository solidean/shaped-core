#include <clean-core/container/span.hh>
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
bool equals(cc::vector<T> const& a, cc::span<T const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}
template <class T>
bool equals(cc::vector<T> const& a, std::initializer_list<T> b)
{
    return equals(a, cc::span<T const>(b));
}

// counts copies and moves, to tell an owning sequence's moves from a borrowing one's copies
struct counted
{
    int value = 0;
    static inline thread_local int copies = 0;
    static inline thread_local int moves = 0;

    explicit counted(int v) : value(v) {}
    counted(counted const& rhs) : value(rhs.value) { ++copies; }
    counted(counted&& rhs) noexcept : value(rhs.value) { ++moves; }
    counted& operator=(counted const&) = default;
    counted& operator=(counted&&) = default;
};

template <class SeqT>
concept can_count_lvalue = requires(SeqT& s) { s.count(); };
} // namespace

TEST("sequence - reductions over a vector")
{
    auto v = make_numbers();

    CHECK(v.values().count() == 8);
    CHECK(v.values().count_if([](int x) { return x > 3; }) == 4);
    CHECK(v.values().any([](int x) { return x == 9; }));
    CHECK(!v.values().any([](int x) { return x == 7; }));
    CHECK(v.values().all([](int x) { return x > 0; }));
    CHECK(v.values().none([](int x) { return x > 9; }));
    CHECK(v.values().contains(5));
    CHECK(!v.values().contains(7));

    auto const idx = v.values().index_of([](int x) { return x == 5; });
    CHECK(idx.has_value());
    CHECK(idx.value() == 4);
    CHECK(!v.values().index_of([](int x) { return x == 7; }).has_value());

    int* const p = v.values().find([](int x) { return x == 9; });
    CHECK(p == &v[5]);
    CHECK(v.values().find([](int x) { return x == 7; }) == nullptr);

    int total = 0;
    v.values().each([&](int x) { total += x; });
    CHECK(total == 31);

    // elements are references, so each can write through them
    v.values().each([](int& x) { x *= 2; });
    CHECK(v[0] == 6);
}

TEST("sequence - empty sequences")
{
    auto const v = cc::vector<int>();

    CHECK(v.values().count() == 0);
    CHECK(!v.values().any([](int) { return true; }));
    CHECK(v.values().all([](int) { return false; }));
    CHECK(!v.values().index_of([](int) { return true; }).has_value());
    CHECK(v.values().sum_from(0) == 0);
    CHECK(v.values().min_or(-1) == -1);
    CHECK(!v.values().try_max().has_value());
    CHECK(!v.values().try_as_non_empty().has_value());
}

TEST("sequence - a callback may take the index first")
{
    auto const v = make_numbers();

    // the index is the position in the sequence the callback is applied to
    CHECK(v.values().count_if([](isize i, int x) { return i % 2 == 0 && x > 2; }) == 3);
    CHECK(v.values().index_of([](isize i, int) { return i == 3; }).value() == 3);

    isize last = -1;
    v.values().each([&](isize i, int) { last = i; });
    CHECK(last == 7);

    // after a filter, a downstream index counts the elements that passed
    auto const positions = v.values().filter([](int x) { return x > 3; }).map([](isize i, int) { return i; }).to_vector();
    CHECK(equals(positions, std::initializer_list<isize>{0, 1, 2, 3}));

    // while the filter's own index counts its input
    CHECK(equals(v.values().filter([](isize i, int) { return i >= 6; }).to_vector(), std::initializer_list<int>{2, 6}));
}

TEST("sequence - try_fold stops on true and ignores void")
{
    auto const v = make_numbers();

    int seen = 0;
    CHECK(v.values().try_fold(
        [&](int x)
        {
            ++seen;
            return x == 4;
        }));
    CHECK(seen == 3);

    seen = 0;
    CHECK(!v.values().try_fold([&](int) { ++seen; }));
    CHECK(seen == 8);
}

TEST("sequence - non-empty reductions")
{
    auto const v = make_numbers();

    CHECK(v.values().assert_non_empty().sum() == 31);
    CHECK(v.values().assert_non_empty().min() == 1);
    CHECK(v.values().assert_non_empty().max() == 9);
    CHECK(v.values().min_or(-1) == 1);
    CHECK(v.values().try_max().value() == 9);

    auto s = v.values().try_as_non_empty();
    REQUIRE(s.has_value());
    CHECK(cc::move(s.value()).sum() == 31);

    // a fixed_span with N > 0 is non-empty without any check
    int const arr[] = {2, 7, 1};
    auto const fs = cc::fixed_span<int const, 3>(arr);
    static_assert(decltype(fs.values())::is_non_empty);
    static_assert(decltype(fs.indices())::is_non_empty);
    CHECK(fs.values().sum() == 10);
    CHECK(fs.values().max() == 7);
    CHECK(fs.indices().sum() == 3);
}

TEST("sequence - non-emptiness propagates and is established at runtime")
{
    auto v = make_numbers();
    using values_t = decltype(v.values());

    static_assert(!values_t::is_non_empty);
    static_assert(decltype(v.values().assert_non_empty())::is_non_empty);
    static_assert(decltype(v.values().assert_non_empty().map([](int x) { return x * 2; }))::is_non_empty);
    static_assert(!decltype(v.values().assert_non_empty().filter([](int) { return true; }))::is_non_empty);
    static_assert(decltype(v.values().assert_non_empty().indices())::is_non_empty);

    // a filter cannot tell it is empty without evaluating, so the first passing element is buffered
    auto big = v.values().filter([](int x) { return x > 4; }).assert_non_empty();
    static_assert(decltype(big)::is_non_empty);
    CHECK(cc::move(big).sum() == 20);

    auto none = v.values().filter([](int x) { return x > 100; }).try_as_non_empty();
    CHECK(!none.has_value());

    auto some = v.values().filter([](int x) { return x < 3; }).try_as_non_empty();
    REQUIRE(some.has_value());
    CHECK(equals(cc::move(some.value()).to_vector(), std::initializer_list<int>{1, 1, 2}));
}

TEST("sequence - sum types")
{
    auto const bytes = cc::vector<u8>{200, 100, 50};

    // u8 + u8 is int, so the sum does not wrap
    static_assert(std::is_same_v<decltype(bytes.values().assert_non_empty().sum()), int>);
    CHECK(bytes.values().assert_non_empty().sum() == 350);

    // sum_from picks the accumulator
    static_assert(std::is_same_v<decltype(bytes.values().sum_from(i64(0))), i64>);
    CHECK(bytes.values().sum_from(i64(1000)) == 1350);

    auto const floats = cc::vector<float>{0.5f, 0.25f};
    CHECK(floats.values().sum_from(0.0f) == 0.75f);
}

TEST("sequence - indices")
{
    auto const v = make_numbers();

    CHECK(v.indices().count() == 8);
    CHECK(v.indices().contains(0));
    CHECK(v.indices().contains(7));
    CHECK(!v.indices().contains(8));
    CHECK(!v.indices().contains(-1));
    CHECK(v.indices().sum_from(isize(0)) == 28);

    // values().indices() is the index sequence of the value sequence
    CHECK(equals(v.values().indices().to_vector(), cc::span<isize const>(v.indices().to_vector())));

    // over an unsized source it counts while consuming
    CHECK(equals(v.values().filter([](int x) { return x == 1; }).indices().to_vector(),
                 std::initializer_list<isize>{0, 1}));

    // a filter forwards contains to its input
    CHECK(v.indices().filter([](isize i) { return i % 2 == 0; }).contains(4));
    CHECK(!v.indices().filter([](isize i) { return i % 2 == 0; }).contains(5));
    CHECK(!v.indices().filter([](isize i) { return i % 2 == 0; }).contains(10));
}

TEST("sequence - map and filter")
{
    auto const v = make_numbers();

    CHECK(equals(v.values().map([](int x) { return x * 10; }).to_vector(),
                 std::initializer_list<int>{30, 10, 40, 10, 50, 90, 20, 60}));
    CHECK(equals(v.values().filter([](int x) { return x % 2 == 0; }).to_vector(), std::initializer_list<int>{4, 2, 6}));
    CHECK(v.values().filter([](int x) { return x % 2 == 0; }).map([](int x) { return x + 1; }).sum_from(0) == 15);

    // a reference result stays a reference, so map projects members with stable elements
    struct point
    {
        int x;
        int y;
    };
    auto pts = cc::vector<point>{{1, 2}, {3, 4}};
    auto ys = pts.values().map([](point& p) -> int& { return p.y; });
    static_assert(decltype(ys)::has_borrowed_elements);
    cc::move(ys).each([](int& y) { y = 0; });
    CHECK(pts[1].y == 0);
}

TEST("sequence - each element is evaluated at most once")
{
    auto const v = make_numbers();

    int calls = 0;
    auto const n = v.values()
                       .map(
                           [&](int x)
                           {
                               ++calls;
                               return x * 2;
                           })
                       .filter([](int x) { return x > 4; })
                       .count();
    CHECK(n == 5);
    CHECK(calls == 8);

    // the same through external iteration
    calls = 0;
    int seen = 0;
    for (int x : v.values()
                     .map(
                         [&](int x)
                         {
                             ++calls;
                             return x * 2;
                         })
                     .filter([](int x) { return x > 4; }))
        seen += x > 0 ? 1 : 0;
    CHECK(seen == 5);
    CHECK(calls == 8);
}

TEST("sequence - range-for")
{
    auto v = make_numbers();

    int total = 0;
    for (int x : v.values())
        total += x;
    CHECK(total == 31);

    for (int& x : v.values().filter([](int x) { return x == 1; }))
        x = 100;
    CHECK(v[1] == 100);
    CHECK(v[3] == 100);
}

TEST("sequence - an rvalue container is owned by its sequence")
{
    using owning_t = decltype(make_numbers().values());
    static_assert(owning_t::has_stable_elements);
    static_assert(!owning_t::has_borrowed_elements);

    CHECK(make_numbers().values().assert_non_empty().sum() == 31);
    CHECK(equals(make_numbers().values().filter([](int x) { return x > 4; }).to_vector(),
                 std::initializer_list<int>{5, 9, 6}));

    // naming it is safe, since the sequence holds the container
    auto s = make_numbers().values();
    CHECK(cc::move(s).count() == 8);

    // materializing moves the elements out
    auto make = []
    {
        auto c = cc::vector<counted>();
        c.emplace_back(1);
        c.emplace_back(2);
        return c;
    };
    counted::copies = 0;
    counted::moves = 0;
    auto const moved = make().values().to_vector();
    CHECK(moved.size() == 2);
    CHECK(counted::copies == 0);

    auto const kept = make();
    counted::copies = 0;
    auto const copied = kept.values().to_vector();
    CHECK(copied.size() == 2);
    CHECK(counted::copies == 2);
}

TEST("sequence - span values")
{
    int arr[] = {1, 2, 3};
    auto const s = cc::span<int>(arr);

    static_assert(decltype(s.values())::has_borrowed_elements);
    CHECK(s.values().count() == 3);
    CHECK(s.values().find([](int x) { return x == 2; }) == &arr[1]);
    CHECK(s.indices().contains(2));
    CHECK(!s.indices().contains(3));
}

TEST("sequence - terminal operations consume and need an rvalue")
{
    auto v = make_numbers();
    using seq_t = decltype(v.values());

    static_assert(!can_count_lvalue<seq_t>);
    static_assert(!std::is_copy_constructible_v<seq_t>);
    static_assert(std::is_move_constructible_v<seq_t>);

    auto s = v.values();
    CHECK(cc::move(s).count() == 8);
}
