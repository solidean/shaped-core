#include "sort-test-types.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace sort_test;

namespace
{
/// What a full sort would produce, computed independently of the range under test.
cc::vector<i32> sorted_copy_of(cc::vector<i32> const& values)
{
    auto copy = values;
    cc::sort(copy);
    return copy;
}
} // namespace

TEST("quickselect - agrees with a full sort at the index")
{
    cc::random rng(21);

    for (auto const p : all_patterns)
    {
        for (auto const n : all_sizes)
        {
            if (n == 0)
                continue;

            SECTION("{} @ {}", to_name(p), n)
            {
                auto const original = make_pattern(p, n, rng);
                auto const expected = sorted_copy_of(original);

                for (isize const idx : {isize(0), n / 2, n - 1})
                {
                    auto values = original;
                    cc::quickselect(values, idx);

                    CHECK(values[idx] == expected[idx]);
                    CHECK(is_permutation_of(original, values));

                    // everything below and above genuinely belongs there, sorted or not
                    for (isize i = 0; i < idx; ++i)
                        CHECK(values[i] <= values[idx]);
                    for (isize i = idx + 1; i < n; ++i)
                        CHECK(values[i] >= values[idx]);
                }
            }
        }
    }
}

TEST("quickselect - stays linear on the adversarial input")
{
    // the two shapes that took the previous implementation quadratic: an adaptive adversary, and mass duplicates
    for (isize const n : {1000, 10000})
    {
        SECTION("adversary @ {}", n)
        {
            auto adv = adversary::create_for(n);

            auto values = cc::vector<isize>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                values[i] = i;

            cc::quickselect(values, n / 2, adv);

            auto const median = adv.values[values[n / 2]];
            for (isize i = 0; i < n / 2; ++i)
                CHECK(adv.values[values[i]] <= median);
            for (isize i = n / 2 + 1; i < n; ++i)
                CHECK(adv.values[values[i]] >= median);

            // a quadratic run would be ~n*n/2 comparisons; a linear one is a small multiple of n
            CHECK(adv.comparison_count < isize(200) * n);
        }
    }
}

TEST("quickselect - a range of nothing but duplicates")
{
    cc::random rng(22);

    for (isize const distinct_values : {1, 2, 4})
    {
        SECTION("{} distinct values", distinct_values)
        {
            isize const n = 20000;
            auto values = cc::vector<i32>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                values[i] = rng.uniform(0, i32(distinct_values) - 1);

            auto const expected = sorted_copy_of(values);
            auto const original = values;

            cc::quickselect(values, n / 3);

            CHECK(values[n / 3] == expected[n / 3]);
            CHECK(is_permutation_of(original, values));
        }
    }
}

TEST("quickselect - by a key")
{
    cc::random rng(23);
    isize const n = 500;

    struct sample
    {
        i32 score = 0;
        i32 id = 0;
    };

    auto values = cc::vector<sample>::create_defaulted(n);
    auto scores = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
    {
        auto const score = rng.uniform(0, 100000);
        values[i] = {.score = score, .id = i32(i)};
        scores[i] = score;
    }

    auto const expected = sorted_copy_of(scores);

    cc::quickselect_by(values, n / 4, &sample::score);
    CHECK(values[n / 4].score == expected[n / 4]);
}

TEST("quickselect - quickselect_range sorts just the window")
{
    cc::random rng(24);
    isize const n = 2000;

    auto const original = make_pattern(pattern::random, n, rng);
    auto const expected = sorted_copy_of(original);

    SECTION("a window in the middle")
    {
        auto values = original;
        isize const idx = 700;
        isize const count = 50;

        cc::quickselect_range(values, idx, count);

        CHECK(is_permutation_of(original, values));
        for (isize i = idx; i < idx + count; ++i)
            CHECK(values[i] == expected[i]);
    }

    SECTION("a window running past the end")
    {
        auto values = original;
        isize const idx = n - 10;

        cc::quickselect_range(values, idx, 1000);

        CHECK(is_permutation_of(original, values));
        for (isize i = idx; i < n; ++i)
            CHECK(values[i] == expected[i]);
    }

    SECTION("the leading window is the top-k idiom")
    {
        auto values = original;
        cc::quickselect_range(values, 0, 20);

        for (isize i = 0; i < 20; ++i)
            CHECK(values[i] == expected[i]);
    }
}

TEST("quickselect - quickselect_range_by")
{
    cc::random rng(25);
    isize const n = 800;

    struct sample
    {
        i32 score = 0;
        i32 id = 0;
    };

    auto values = cc::vector<sample>::create_defaulted(n);
    auto scores = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
    {
        auto const score = rng.uniform(0, 100000);
        values[i] = {.score = score, .id = i32(i)};
        scores[i] = score;
    }

    auto const expected = sorted_copy_of(scores);
    cc::quickselect_range_by(values, 0, 10, &sample::score);

    for (isize i = 0; i < 10; ++i)
        CHECK(values[i].score == expected[i]);
}

TEST("quickselect - a single-element range")
{
    auto values = cc::vector<i32>{7};
    cc::quickselect(values, 0);
    CHECK(values[0] == 7);
}
