#include <clean-core/algorithm/search.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// 0..n-1 doubled, so every value has a known window of exactly 2 and the odd numbers are known absent.
cc::vector<i32> evens_twice(isize n)
{
    auto r = cc::vector<i32>::create_with_capacity(n * 2);
    for (isize i = 0; i < n; ++i)
    {
        r.push_back(i32(i) * 2);
        r.push_back(i32(i) * 2);
    }
    return r;
}
} // namespace

TEST("partition_point - finds where a monotone predicate turns over")
{
    auto const values = cc::vector<i32>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    for (isize const threshold : {isize(0), isize(1), isize(5), isize(9), isize(10)})
    {
        SECTION("threshold {}", threshold)
        {
            auto const idx = cc::partition_point(values, [&](i32 v) { return isize(v) < threshold; });
            CHECK(idx == threshold);
        }
    }
}

TEST("partition_point - an always-true and an always-false predicate")
{
    auto const values = cc::vector<i32>{1, 2, 3};

    CHECK(cc::partition_point(values, [](i32) { return true; }) == 3);
    CHECK(cc::partition_point(values, [](i32) { return false; }) == 0);
}

TEST("partition_point - an empty range")
{
    auto const values = cc::vector<i32>();
    CHECK(cc::partition_point(values, [](i32) { return true; }) == 0);
}

TEST("search - the bounds agree with a linear scan, at every size")
{
    for (isize const n : {isize(0), isize(1), isize(2), isize(5), isize(64), isize(1000)})
    {
        SECTION("n = {}", n)
        {
            auto const values = evens_twice(n);
            isize const size = isize(values.size());

            // probe every present value, every absent one between them, and both ends
            for (i32 probe = -1; probe <= i32(n) * 2; ++probe)
            {
                isize expected_first = size;
                for (isize i = 0; i < size; ++i)
                    if (!(values[i] < probe))
                    {
                        expected_first = i;
                        break;
                    }

                isize expected_last = size;
                for (isize i = 0; i < size; ++i)
                    if (probe < values[i])
                    {
                        expected_last = i;
                        break;
                    }

                CHECK(cc::first_at_least_in_sorted(values, probe) == expected_first);
                CHECK(cc::first_greater_in_sorted(values, probe) == expected_last);
            }
        }
    }
}

TEST("find_in_sorted - present and absent")
{
    auto const values = evens_twice(50);

    for (i32 v = 0; v < 100; v += 2)
    {
        auto const found = cc::find_in_sorted(values, v);
        CHECK(found.has_value());
        CHECK(values[found.value()] == v);
    }

    for (i32 v = -3; v < 105; v += 2)
        CHECK(!cc::find_in_sorted(values, v).has_value());
}

TEST("find_in_sorted - an empty range finds nothing")
{
    auto const values = cc::vector<i32>();
    CHECK(!cc::find_in_sorted(values, 0).has_value());
}

TEST("find_range_in_sorted - the whole run of equivalent elements")
{
    auto const values = evens_twice(50);

    for (i32 v = 0; v < 100; v += 2)
    {
        auto const window = cc::find_range_in_sorted(values, v);
        CHECK(window.size == 2);
        CHECK(values[window.offset] == v);
        CHECK(values[window.offset + 1] == v);
    }
}

TEST("find_range_in_sorted - size 0 means absent, and the offset is the insertion point")
{
    auto const values = cc::vector<i32>{10, 20, 30};

    for (auto const [probe, expected_offset] : {cc::pair<i32, isize>{5, 0}, {15, 1}, {25, 2}, {35, 3}})
    {
        auto const window = cc::find_range_in_sorted(values, probe);
        CHECK(window.size == 0);
        CHECK(window.offset == expected_offset);
    }
}

TEST("find_range_in_sorted - a range of nothing but one value")
{
    auto const values = cc::vector<i32>(cc::vector<i32>::create_filled(100, 7));

    auto const window = cc::find_range_in_sorted(values, 7);
    CHECK(window.offset == 0);
    CHECK(window.size == 100);

    CHECK(cc::find_range_in_sorted(values, 6).offset == 0);
    CHECK(cc::find_range_in_sorted(values, 8).offset == 100);
}

TEST("search - a custom comparator, matching the order the range is in")
{
    auto values = cc::vector<i32>{5, 4, 3, 2, 1};
    CHECK(cc::is_sorted(values, cc::default_greater{}));

    CHECK(cc::first_at_least_in_sorted(values, 3, cc::default_greater{}) == 2);
    CHECK(cc::first_greater_in_sorted(values, 3, cc::default_greater{}) == 3);
    CHECK(cc::find_in_sorted(values, 3, cc::default_greater{}).value() == 2);
    CHECK(!cc::find_in_sorted(values, 9, cc::default_greater{}).has_value());
}

TEST("search - agrees with a sort on random input")
{
    cc::random rng(31);

    for (isize const n : {isize(3), isize(100), isize(5000)})
    {
        SECTION("n = {}", n)
        {
            auto values = cc::vector<i32>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                values[i] = rng.uniform(0, 50); // deliberately narrow, so runs of equivalents are long

            cc::sort(values);

            for (i32 probe = -1; probe <= 51; ++probe)
            {
                auto const window = cc::find_range_in_sorted(values, probe);

                isize count = 0;
                for (auto const v : values)
                    if (v == probe)
                        ++count;

                CHECK(window.size == count);
                CHECK(cc::find_in_sorted(values, probe).has_value() == (count > 0));

                // the window is exactly where the equivalents are
                for (isize i = 0; i < window.size; ++i)
                    CHECK(values[window.offset + i] == probe);
            }
        }
    }
}

TEST("search - a heterogeneous probe, comparing string against string_view")
{
    auto values = cc::vector<cc::string>{"alpha", "beta", "delta", "gamma"};
    CHECK(cc::is_sorted(values, [](auto const& a, auto const& b) { return cc::string_view(a) < cc::string_view(b); }));

    auto const less = [](auto const& a, auto const& b) { return cc::string_view(a) < cc::string_view(b); };

    CHECK(cc::find_in_sorted(values, cc::string_view("delta"), less).value() == 2);
    CHECK(!cc::find_in_sorted(values, cc::string_view("epsilon"), less).has_value());
    CHECK(cc::first_at_least_in_sorted(values, cc::string_view("epsilon"), less) == 3);
}
