#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/sequence.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

#include <compare>
#include <vector>

using namespace cc::primitive_defines;

TEST("sequence - equals, starts_with, mismatch")
{
    auto const a = cc::vector<int>{1, 2, 3};

    CHECK(a.values().equals(cc::vector<int>{1, 2, 3}));
    CHECK(!a.values().equals(cc::vector<int>{1, 2}));
    CHECK(!a.values().equals(cc::vector<int>{1, 2, 3, 4}));
    CHECK(!a.values().equals(cc::vector<int>{1, 5, 3}));
    CHECK(a.values().equals(cc::iota(1, 4)));
    CHECK(a.values().filter([](int) { return true; }).equals(std::vector<int>{1, 2, 3}));
    CHECK(cc::vector<int>().values().equals(cc::vector<int>()));

    CHECK(a.values().starts_with(cc::vector<int>{1, 2}));
    CHECK(a.values().starts_with(cc::vector<int>()));
    CHECK(!a.values().starts_with(cc::vector<int>{2}));
    CHECK(!a.values().starts_with(cc::vector<int>{1, 2, 3, 4}));

    CHECK(!a.values().mismatch(cc::vector<int>{1, 2, 3}).has_value());
    CHECK(a.values().mismatch(cc::vector<int>{1, 9, 3}).value() == 1);
    CHECK(a.values().mismatch(cc::vector<int>{1, 2}).value() == 2);
    CHECK(a.values().mismatch(cc::vector<int>{1, 2, 3, 4}).value() == 3);
}

TEST("sequence - compare is lexicographic")
{
    auto const a = cc::vector<int>{1, 2, 3};

    CHECK((a.values().compare(cc::vector<int>{1, 2, 3}) == 0));
    CHECK((a.values().compare(cc::vector<int>{1, 2, 4}) < 0));
    CHECK((a.values().compare(cc::vector<int>{1, 1, 9}) > 0));
    CHECK((a.values().compare(cc::vector<int>{1, 2}) > 0)); // a proper prefix orders first
    CHECK((a.values().compare(cc::vector<int>{1, 2, 3, 0}) < 0));
    static_assert(std::is_same_v<decltype(a.values().compare(a)), std::strong_ordering>);

    // floats compare partially
    auto const f = cc::vector<float>{1.0f, 2.0f};
    static_assert(std::is_same_v<decltype(f.values().compare(f)), std::partial_ordering>);
    CHECK((f.values().compare(cc::vector<float>{1.0f, 3.0f}) < 0));
}

TEST("sequence - is_sorted")
{
    CHECK((cc::vector<int>{1, 2, 2, 5}.values().is_sorted()));
    CHECK((!cc::vector<int>{1, 3, 2}.values().is_sorted()));
    CHECK(cc::vector<int>().values().is_sorted());
    CHECK(cc::vector<int>{4}.values().is_sorted());
    CHECK((cc::vector<int>{5, 3, 1}.values().is_sorted_by([](int x) { return -x; })));
}

TEST("sequence - min_max, index_of_min / max, nth")
{
    auto const v = cc::vector<int>{3, 1, 4, 1, 5, 9, 2, 6};

    auto const [lo, hi] = v.values().assert_non_empty().min_max();
    CHECK(lo == 1);
    CHECK(hi == 9);
    CHECK(!cc::vector<int>().values().try_min_max().has_value());
    CHECK(cc::single(7).min_max().max == 7);

    CHECK(v.values().index_of_min().value() == 1); // the first of equal ones
    CHECK(v.values().index_of_max().value() == 5);
    CHECK(!cc::vector<int>().values().index_of_max().has_value());

    CHECK(v.values().nth(4) == 5);
    CHECK(!v.values().try_nth(8).has_value());
    CHECK(!v.values().try_nth(-1).has_value());
    CHECK(v.values().filter([](int x) { return x > 2; }).nth(2) == 5);
}

TEST("sequence - count_by, partition, to_map, to_set")
{
    auto const v = cc::vector<int>{3, 1, 4, 1, 5, 9, 2, 6};

    auto const by_parity = v.values().count_by([](int x) { return x % 2; });
    CHECK(by_parity.get(0) == 3);
    CHECK(by_parity.get(1) == 5);

    auto const [small, large] = v.values().partition([](int x) { return x < 4; });
    CHECK(small.size() == 4);
    CHECK(large.size() == 4);
    CHECK(small[0] == 3);
    CHECK(large[0] == 4);

    auto const squares = v.values().to_map([](int x) { return x; }, [](int x) { return x * x; });
    CHECK(squares.size() == 7);
    CHECK(squares.get(9) == 81);

    auto const distinct = v.values().to_set();
    CHECK(distinct.size() == 7);
    CHECK(distinct.contains(9));
}

TEST("sequence - join and cc::format")
{
    auto const v = cc::vector<int>{1, 2, 3};

    CHECK(v.values().join(", ") == "1, 2, 3");
    CHECK(cc::vector<int>().values().join(", ") == "");
    CHECK((cc::vector<cc::string_view>{"a", "b"}.values().join("/") == "a/b"));
    CHECK(v.values().map([](int x) { return x * 10; }).join("") == "102030");

    CHECK(cc::format("{}", v.values()) == "[1, 2, 3]");
    CHECK(cc::format("{}", cc::vector<int>().values()) == "[]");
    CHECK(cc::format("{:x}", cc::vector<int>{10, 255}.values()) == "[a, ff]");
    CHECK(cc::format("{:.1f}", cc::vector<float>{0.5f, 1.25f}.values().map([](float x) { return x * 2; }))
          == "[1.0, 2.5]");
    CHECK(cc::format("{}", cc::zip(v, cc::vector<int>{4, 5, 6})) == "[(1, 4), (2, 5), (3, 6)]");
    CHECK(cc::format("{}", v.values().pairwise()) == "[(1, 2), (2, 3)]");
}
