#include "sort-test-types.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/algorithm/sort_async.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace sort_test;

// cc::sort_async's own tests, deliberately NOT gated on CC_HAS_THREADS, for the same reason async-pool-test.cc
// is not: the pool exists on every platform and falls back to driving inline, so these pin that a parallel-shaped
// API gives the same ANSWERS with or without threads — which is the only claim the fallback makes.
//
// Every case passes a tiny cutoff, so the real recursion runs hundreds of tasks over a few thousand elements and
// the whole file stays in the millisecond range.
// The shipped default would need a multi-second range to fan out at all, which is exactly why cutoff is a parameter.
//
// sort_test::adversary is deliberately absent: it is a stateful, mutating comparator, and sort_async copies the
// comparator into every task, which makes its counts meaningless.
// sort-test.cc owns that coverage for the sequential driver — and after the sort_step extraction that is the very
// same code.

namespace
{
/// Drives the graph to completion on a real pool, which is also the inline path under -DSC_THREADS=OFF.
void drive(cc::shared_async<cc::unit> const& root)
{
    cc::async_thread_pool pool(4);
    (void)pool.blocking_get(root);
}

/// Wide enough that the pivot is bound by reference and the block partition is off, i.e. the other partition path.
struct wide
{
    i64 key = 0;
    i64 padding[7] = {};
};
} // namespace

TEST("sort_async - lands on exactly what cc::sort produces")
{
    // The partition is sequential and each task owns a disjoint subrange, so the swap sequence is a function of
    // the input alone and not of the schedule.
    // Equality element-for-element therefore holds, and pins far more than is_sorted would: that the parallel
    // driver takes the SAME decisions as sort_loop.
    cc::random rng(51);

    for (auto const p : all_patterns)
    {
        SECTION("{}", to_name(p))
        {
            for (isize const n : {isize(0), isize(1), isize(15), isize(16), isize(17), isize(1000), isize(20000)})
            {
                auto const original = make_pattern(p, n, rng);

                auto expected = original;
                cc::sort(expected);

                auto values = original;
                drive(cc::sort_async_ex(0, n, cc::as_index_swap_range(values), cc::default_less{}, isize(16)));

                CHECK(isize(values.size()) == n);
                for (isize i = 0; i < n; ++i)
                    CHECK(values[i] == expected[i]);
            }
        }
    }
}

TEST("sort_async - sorts a large random range, checked without reference to cc::sort")
{
    cc::random rng(52);
    isize const n = 50000;

    auto const original = make_pattern(pattern::random, n, rng);
    auto values = original;

    drive(cc::sort_async_ex(0, n, cc::as_index_swap_range(values), cc::default_less{}, isize(64)));

    CHECK(cc::is_sorted(values));
    CHECK(is_permutation_of(original, values));
}

TEST("sort_async - the shipped default cutoff spawns nothing and still sorts")
{
    cc::random rng(53);
    auto const original = make_pattern(pattern::random, 1000, rng);

    auto values = original;
    drive(cc::sort_async(values));

    CHECK(cc::is_sorted(values));
    CHECK(is_permutation_of(original, values));
}

TEST("sort_async - a wide element type, which takes the non-blockwise partition")
{
    cc::random rng(54);
    isize const n = 5000;

    auto const source = make_pattern(pattern::random, n, rng);
    auto values = cc::vector<wide>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        values[i] = {.key = i64(source[i])};

    auto expected = values;
    cc::sort(expected, [](wide const& a, wide const& b) { return a.key < b.key; });

    drive(cc::sort_async_ex(
        0, n, cc::as_index_swap_range(values), [](wide const& a, wide const& b) { return a.key < b.key; }, isize(16)));

    for (isize i = 0; i < n; ++i)
        CHECK(values[i].key == expected[i].key);
}

TEST("sort_async - a by-value comparator survives the copy into every task")
{
    cc::random rng(55);
    isize const n = 8000;

    auto const original = make_pattern(pattern::random, n, rng);

    SECTION("cc::default_greater")
    {
        auto values = original;
        drive(cc::sort_async_ex(0, n, cc::as_index_swap_range(values), cc::default_greater{}, isize(32)));
        CHECK(cc::is_sorted(values, cc::default_greater{}));
    }

    SECTION("a stateless lambda")
    {
        auto values = original;
        auto const by_magnitude = [](i32 a, i32 b) { return (a < 0 ? -a : a) < (b < 0 ? -b : b); };

        drive(cc::sort_async_ex(0, n, cc::as_index_swap_range(values), by_magnitude, isize(32)));
        CHECK(cc::is_sorted(values, by_magnitude));
    }
}

TEST("sort_async - composes as a dependency of other async work")
{
    cc::random rng(56);
    isize const n = 5000;

    auto values = make_pattern(pattern::random, n, rng);

    auto const sorted = cc::sort_async_ex(0, n, cc::as_index_swap_range(values), cc::default_less{}, isize(32));

    // the continuation must see a fully sorted range, which is the whole reason this returns an async
    // the dependency is unwrapped to its plain value before the continuation runs, hence the cc::unit parameter
    auto const checked = cc::make_async_lazy([&values](cc::unit) { return cc::is_sorted(values) ? 1 : 0; }, sorted);

    cc::async_thread_pool pool(4);
    CHECK(pool.blocking_get(checked) == 1);
}

TEST("sort_async - two disjoint sorts in one graph")
{
    cc::random rng(57);
    isize const n = 4000;

    auto a = make_pattern(pattern::random, n, rng);
    auto b = make_pattern(pattern::reverse, n, rng);

    auto const sa = cc::sort_async_ex(0, n, cc::as_index_swap_range(a), cc::default_less{}, isize(32));
    auto const sb = cc::sort_async_ex(0, n, cc::as_index_swap_range(b), cc::default_less{}, isize(32));
    auto const both = cc::make_async_lazy(
        [&a, &b](cc::unit, cc::unit) { return (cc::is_sorted(a) && cc::is_sorted(b)) ? 1 : 0; }, sa, sb);

    cc::async_thread_pool pool(4);
    CHECK(pool.blocking_get(both) == 1);
}

TEST("sort_async - completes on a singlethreaded scheduler, with no pool installed")
{
    cc::random rng(58);
    isize const n = 3000;

    auto const original = make_pattern(pattern::random, n, rng);
    auto values = original;

    auto const sorted = cc::sort_async_ex(0, n, cc::as_index_swap_range(values), cc::default_less{}, isize(16));
    cc::async_blocking_get_singlethreaded(sorted);

    CHECK(cc::is_sorted(values));
    CHECK(is_permutation_of(original, values));
}

TEST("sort_async - keeps parallel ranges in step through the seam")
{
    cc::random rng(59);
    isize const n = 6000;

    auto keys = make_pattern(pattern::random, n, rng);
    auto tags = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        tags[i] = keys[i]; // the tag mirrors its key, so any drift shows up as a mismatch

    drive(cc::sort_async_ex(0, n, cc::as_index_swap_range_multi(keys, tags), cc::default_less{}, isize(32)));

    CHECK(cc::is_sorted(keys));
    for (isize i = 0; i < n; ++i)
        CHECK(keys[i] == tags[i]);
}
