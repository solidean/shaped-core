#include <clean-core/algorithm/permutation.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::vector<i32> iota(isize n)
{
    auto r = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        r[i] = i32(i);
    return r;
}

/// A uniformly random permutation of 0..n-1, built by Fisher-Yates so it never depends on the sort.
cc::vector<i32> random_permutation(isize n, cc::random& rng)
{
    auto r = iota(n);
    for (isize i = n - 1; i > 0; --i)
        cc::swap(r[i], r[rng.uniform(isize(0), i)]);
    return r;
}
} // namespace

TEST("reverse - flips a range of any size")
{
    for (isize const n : {isize(0), isize(1), isize(2), isize(3), isize(17)})
    {
        SECTION("n = {}", n)
        {
            auto values = iota(n);
            cc::reverse(values);

            CHECK(isize(values.size()) == n);
            for (isize i = 0; i < n; ++i)
                CHECK(values[i] == i32(n - 1 - i));
        }
    }
}

TEST("reverse - twice is the identity")
{
    auto values = iota(41);
    cc::reverse(values);
    cc::reverse(values);

    for (isize i = 0; i < 41; ++i)
        CHECK(values[i] == i32(i));
}

TEST("rotate - brings the element at count to the front")
{
    isize const n = 10;

    for (isize const count : {isize(0), isize(1), isize(3), isize(9)})
    {
        SECTION("count = {}", count)
        {
            auto values = iota(n);
            cc::rotate(values, count);

            for (isize i = 0; i < n; ++i)
                CHECK(values[i] == i32((i + count) % n));
        }
    }
}

TEST("rotate - the count is normalized, so a full turn and a negative one are legal")
{
    isize const n = 7;

    SECTION("a full turn is the identity")
    {
        auto values = iota(n);
        cc::rotate(values, n);
        for (isize i = 0; i < n; ++i)
            CHECK(values[i] == i32(i));
    }

    SECTION("more than a full turn wraps")
    {
        auto values = iota(n);
        cc::rotate(values, n + 2);
        for (isize i = 0; i < n; ++i)
            CHECK(values[i] == i32((i + 2) % n));
    }

    SECTION("rotating left by -1 is rotating right by 1")
    {
        auto values = iota(n);
        cc::rotate(values, -1);
        CHECK(values[0] == i32(n - 1));
        for (isize i = 1; i < n; ++i)
            CHECK(values[i] == i32(i - 1));
    }
}

TEST("rotate - an empty and a single-element range")
{
    auto empty = cc::vector<i32>();
    cc::rotate(empty, 3);
    CHECK(empty.empty());

    auto one = cc::vector<i32>{5};
    cc::rotate(one, 3);
    CHECK(one[0] == 5);
}

TEST("apply_permutation - position i draws from indices[i]")
{
    auto values = cc::vector<i32>{10, 11, 12};
    auto order = cc::vector<i32>{2, 0, 1};

    cc::apply_permutation(values, order);

    CHECK(values[0] == 12);
    CHECK(values[1] == 10);
    CHECK(values[2] == 11);
}

TEST("apply_permutation - comes out as the identity, which is the documented cost")
{
    auto values = iota(20);
    cc::random rng(41);
    auto order = random_permutation(20, rng);

    cc::apply_permutation(values, order);

    for (isize i = 0; i < 20; ++i)
        CHECK(order[i] == i32(i));
}

TEST("apply_permutation - agrees with a gather on random permutations")
{
    cc::random rng(42);

    for (isize const n : {isize(0), isize(1), isize(2), isize(17), isize(2000)})
    {
        SECTION("n = {}", n)
        {
            auto const original = [&]
            {
                auto r = cc::vector<i32>::create_defaulted(n);
                for (isize i = 0; i < n; ++i)
                    r[i] = rng.uniform(0, 1000000);
                return r;
            }();

            auto order = random_permutation(n, rng);

            // what a gather with a scratch buffer would produce, computed before order is consumed
            auto expected = cc::vector<i32>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                expected[i] = original[isize(order[i])];

            auto values = original;
            cc::apply_permutation(values, order);

            CHECK(isize(values.size()) == n);
            for (isize i = 0; i < n; ++i)
                CHECK(values[i] == expected[i]);
        }
    }
}

TEST("apply_permutation - keeps parallel ranges in step through the seam")
{
    isize const n = 6;
    auto keys = cc::vector<i32>{50, 10, 40, 20, 30, 0};
    auto tags = cc::vector<i32>{5, 1, 4, 2, 3, 0};

    auto order = iota(n);
    cc::sort_indices(order, keys);

    cc::apply_permutation_ex(n, order, cc::as_index_swap_range_multi(keys, tags));

    for (isize i = 0; i < n; ++i)
    {
        CHECK(keys[i] == i32(i) * 10);
        CHECK(tags[i] == i32(i)); // the tag rode along, so the two never fell out of step
    }
}

TEST("invert_permutation - inverting twice is the identity")
{
    cc::random rng(43);

    for (isize const n : {isize(0), isize(1), isize(2), isize(31), isize(1000)})
    {
        SECTION("n = {}", n)
        {
            auto const original = random_permutation(n, rng);

            auto order = original;
            cc::invert_permutation(order);
            cc::invert_permutation(order);

            CHECK(isize(order.size()) == n);
            for (isize i = 0; i < n; ++i)
                CHECK(order[i] == original[i]);
        }
    }
}

TEST("invert_permutation - the inverse undoes the permutation")
{
    cc::random rng(44);
    isize const n = 200;

    auto const original = random_permutation(n, rng);

    auto inverse = original;
    cc::invert_permutation(inverse);

    for (isize i = 0; i < n; ++i)
        CHECK(isize(inverse[isize(original[i])]) == i);
}

TEST("invert_permutation - turns a sort order into ranks")
{
    auto const values = cc::vector<i32>{30, 10, 20};

    auto order = iota(3);
    cc::sort_indices(order, values); // order = {1, 2, 0}: position 0 draws from index 1

    cc::invert_permutation(order); // ranks: element 0 sorts to position 2, element 1 to 0, element 2 to 1

    CHECK(order[0] == 2);
    CHECK(order[1] == 0);
    CHECK(order[2] == 1);
}

TEST("partition_stable - splits the range and keeps the order within each block")
{
    cc::random rng(45);

    for (isize const n : {isize(0), isize(1), isize(2), isize(17), isize(3000)})
    {
        SECTION("n = {}", n)
        {
            struct sample
            {
                bool goes_right = false;
                i32 arrival = 0;
            };

            auto values = cc::vector<sample>::create_defaulted(n);
            isize expected_left = 0;
            for (isize i = 0; i < n; ++i)
            {
                bool const right = rng.uniform(0, 3) == 0; // deliberately lopsided
                values[i] = {.goes_right = right, .arrival = i32(i)};
                if (!right)
                    ++expected_left;
            }

            auto const cut = cc::partition_stable(values, [](sample const& s) { return s.goes_right; });

            CHECK(cut == expected_left);
            for (isize i = 0; i < n; ++i)
                CHECK(values[i].goes_right == (i >= cut));

            // arrival order preserved inside each block, which is the whole difference from partition_by
            for (isize i = 1; i < cut; ++i)
                CHECK(values[i - 1].arrival < values[i].arrival);
            for (isize i = cut + 1; i < n; ++i)
                CHECK(values[i - 1].arrival < values[i].arrival);
        }
    }
}

TEST("partition_stable - all-left and all-right ranges")
{
    auto all_left = cc::vector<i32>{1, 2, 3};
    CHECK(cc::partition_stable(all_left, [](i32) { return false; }) == 3);
    CHECK(all_left[0] == 1);
    CHECK(all_left[2] == 3);

    auto all_right = cc::vector<i32>{1, 2, 3};
    CHECK(cc::partition_stable(all_right, [](i32) { return true; }) == 0);
    CHECK(all_right[0] == 1);
    CHECK(all_right[2] == 3);
}

TEST("partition_stable - the predicate is evaluated exactly once per element")
{
    isize const n = 1000;
    auto values = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        values[i] = i32(i);

    isize calls = 0;
    auto const cut = cc::partition_stable(values,
                                          [&](i32 v)
                                          {
                                              ++calls;
                                              return v % 3 == 0;
                                          });

    CHECK(calls == n);
    CHECK(cut == n - (n + 2) / 3);
}

TEST("partition_stable - keeps parallel ranges in step through the seam")
{
    auto flags = cc::vector<i32>{1, 0, 1, 0, 0, 1};
    auto tags = cc::vector<i32>{0, 1, 2, 3, 4, 5};

    auto const range = cc::as_index_swap_range_multi(flags, tags);
    auto const cut = cc::partition_stable_ex(0, 6, [&](isize i) { return flags[i] != 0; }, range);

    CHECK(cut == 3);
    CHECK(tags[0] == 1);
    CHECK(tags[1] == 3);
    CHECK(tags[2] == 4);
    CHECK(tags[3] == 0);
    CHECK(tags[4] == 2);
    CHECK(tags[5] == 5);
}
