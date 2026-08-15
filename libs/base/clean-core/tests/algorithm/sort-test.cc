#include "sort-test-types.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;
using namespace sort_test;

TEST("sort - every pattern at every size")
{
    cc::random rng(9001);

    for (auto const p : all_patterns)
    {
        for (auto const n : all_sizes)
        {
            SECTION("{} @ {}", to_name(p), n)
            {
                auto const original = make_pattern(p, n, rng);

                auto values = original;
                cc::sort(values);

                CHECK(cc::is_sorted(values));
                CHECK(is_permutation_of(original, values));
            }
        }
    }
}

TEST("sort - descending is the exact reverse order")
{
    cc::random rng(4);
    auto const original = make_pattern(pattern::random, 500, rng);

    auto ascending = original;
    cc::sort(ascending);

    auto descending = original;
    cc::sort_descending(descending);

    REQUIRE(cc::is_sorted(descending, cc::default_greater{}));
    for (isize i = 0; i < isize(original.size()); ++i)
        CHECK(ascending[i] == descending[isize(original.size()) - 1 - i]);
}

namespace
{
struct entry
{
    i32 key = 0;
    i32 payload = 0;

    i32 get_key() const { return key; }
};

/// Too wide to snapshot the pivot into a local, so the partition holds it by reference and skips block partitioning.
struct wide
{
    i64 key = 0;
    i64 padding[7] = {};

    friend bool operator<(wide const& a, wide const& b) { return a.key < b.key; }
};
static_assert(sizeof(wide) > 32);
} // namespace

TEST("sort - by a key")
{
    cc::random rng(5);
    auto entries = cc::vector<entry>::create_defaulted(300);
    for (isize i = 0; i < 300; ++i)
        entries[i] = {.key = rng.uniform(0, 1000), .payload = i32(i)};

    SECTION("member pointer")
    {
        auto values = entries;
        cc::sort_by(values, &entry::key);
        CHECK(cc::is_sorted_by(values, &entry::key));
    }

    SECTION("member function pointer")
    {
        auto values = entries;
        cc::sort_by(values, &entry::get_key);
        CHECK(cc::is_sorted_by(values, &entry::get_key));
    }

    SECTION("lambda, descending")
    {
        auto values = entries;
        cc::sort_by_descending(values, [](entry const& e) { return e.key; });
        CHECK(cc::is_sorted_by(values, &entry::key, cc::default_greater{}));
    }
}

TEST("sort - sort_multi keeps the payload ranges in step")
{
    cc::random rng(6);
    isize const n = 400;

    auto keys = cc::vector<i32>::create_defaulted(n);
    auto payload_a = cc::vector<i32>::create_defaulted(n);
    auto payload_b = cc::vector<cc::string>::create_defaulted(n);

    for (isize i = 0; i < n; ++i)
    {
        keys[i] = rng.uniform(0, 50); // duplicates on purpose
        payload_a[i] = keys[i] * 7;
        payload_b[i] = cc::to_string(keys[i] * 7);
    }

    cc::sort_multi(cc::default_less{}, keys, payload_a, payload_b);

    REQUIRE(cc::is_sorted(keys));
    for (isize i = 0; i < n; ++i)
    {
        CHECK(payload_a[i] == keys[i] * 7);
        CHECK(payload_b[i] == cc::to_string(keys[i] * 7));
    }
}

TEST("sort - sort_multi_ascending and _descending")
{
    auto keys = cc::vector<i32>{5, 1, 4, 2, 3};
    auto payload = cc::vector<i32>{50, 10, 40, 20, 30};

    cc::sort_multi_ascending(keys, payload);
    CHECK(cc::is_sorted(keys));
    for (isize i = 0; i < 5; ++i)
        CHECK(payload[i] == keys[i] * 10);

    cc::sort_multi_descending(keys, payload);
    CHECK(cc::is_sorted(keys, cc::default_greater{}));
    for (isize i = 0; i < 5; ++i)
        CHECK(payload[i] == keys[i] * 10);
}

TEST("sort - sort_multi_by sees one element of every range")
{
    auto xs = cc::vector<i32>{3, 0, 5, 1};
    auto ys = cc::vector<i32>{4, 0, 12, 1};

    // orders by squared length, so the pairs must stay together
    cc::sort_multi_by([](i32 x, i32 y) { return x * x + y * y; }, cc::default_less{}, xs, ys);

    auto const expected_xs = cc::vector<i32>{0, 1, 3, 5};
    auto const expected_ys = cc::vector<i32>{0, 1, 4, 12};
    for (isize i = 0; i < 4; ++i)
    {
        CHECK(xs[i] == expected_xs[i]);
        CHECK(ys[i] == expected_ys[i]);
    }
}

TEST("sort - sort_indices orders the keys and breaks ties on the index")
{
    cc::random rng(7);
    isize const n = 300;

    auto keys = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        keys[i] = rng.uniform(0, 10); // heavy duplication, so the tiebreak actually runs

    auto order = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        order[i] = i32(i);

    cc::sort_indices(order, keys);

    // the keys come out ordered, and equal keys keep their original index order
    for (isize i = 1; i < n; ++i)
    {
        CHECK(keys[order[i - 1]] <= keys[order[i]]);
        if (keys[order[i - 1]] == keys[order[i]])
            CHECK(order[i - 1] < order[i]);
    }

    // and it is a permutation of 0..n-1
    auto seen = cc::vector<bool>::create_filled(n, false);
    for (isize i = 0; i < n; ++i)
        seen[order[i]] = true;
    for (isize i = 0; i < n; ++i)
        CHECK(seen[i]);
}

TEST("sort - sort_by_cached_key evaluates the key once per element")
{
    cc::random rng(8);
    isize const n = 400;

    auto values = cc::vector<i32>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        values[i] = rng.uniform(0, 100000);

    isize cached_calls = 0;
    auto cached = values;
    cc::sort_by_cached_key(cached,
                           [&](i32 v)
                           {
                               ++cached_calls;
                               return -v;
                           });

    CHECK(cached_calls == n);
    CHECK(cc::is_sorted(cached, cc::default_greater{}));

    // the plain form recomputes per comparison, which is the reason the cached one exists
    isize plain_calls = 0;
    auto plain = values;
    cc::sort_by(plain,
                [&](i32 v)
                {
                    ++plain_calls;
                    return -v;
                });

    CHECK(plain_calls > n);
    for (isize i = 0; i < n; ++i)
        CHECK(plain[i] == cached[i]);
}

TEST("sort - is_sorted and is_strictly_sorted differ on duplicates")
{
    auto const with_duplicates = cc::vector<i32>{1, 2, 2, 3};
    auto const distinct = cc::vector<i32>{1, 2, 3, 4};
    auto const unsorted = cc::vector<i32>{1, 3, 2};

    CHECK(cc::is_sorted(with_duplicates));
    CHECK(!cc::is_strictly_sorted(with_duplicates));

    CHECK(cc::is_sorted(distinct));
    CHECK(cc::is_strictly_sorted(distinct));

    CHECK(!cc::is_sorted(unsorted));
    CHECK(!cc::is_strictly_sorted(unsorted));

    // an empty and a single-element range are both, vacuously
    auto const empty = cc::vector<i32>();
    auto const single = cc::vector<i32>{7};
    CHECK(cc::is_sorted(empty));
    CHECK(cc::is_strictly_sorted(empty));
    CHECK(cc::is_sorted(single));
    CHECK(cc::is_strictly_sorted(single));
}

TEST("sort - is_sorted_by")
{
    auto const entries = cc::vector<entry>{{.key = 1, .payload = 9}, {.key = 5, .payload = 0}};
    CHECK(cc::is_sorted_by(entries, &entry::key));
    CHECK(!cc::is_sorted_by(entries, &entry::payload));
    CHECK(cc::is_strictly_sorted_by(entries, &entry::key));
}

TEST("sort - partition_by splits and reports the boundary")
{
    cc::random rng(11);

    for (auto const n : all_sizes)
    {
        SECTION("n = {}", n)
        {
            auto const original = make_pattern(pattern::random, n, rng);

            SECTION("mixed predicate")
            {
                auto values = original;
                auto const boundary = cc::partition_by(values, [](i32 v) { return v > 0; });

                REQUIRE(0 <= boundary);
                REQUIRE(boundary <= n);
                for (isize i = 0; i < boundary; ++i)
                    CHECK(values[i] <= 0);
                for (isize i = boundary; i < n; ++i)
                    CHECK(values[i] > 0);
                CHECK(is_permutation_of(original, values));
            }

            SECTION("everything left")
            {
                auto values = original;
                CHECK(cc::partition_by(values, [](i32) { return false; }) == n);
                CHECK(is_permutation_of(original, values));
            }

            SECTION("everything right")
            {
                auto values = original;
                CHECK(cc::partition_by(values, [](i32) { return true; }) == 0);
                CHECK(is_permutation_of(original, values));
            }
        }
    }
}

TEST("sort - the adversarial comparator still terminates and sorts")
{
    // McIlroy's antiqsort defeats the median-of-three pivot at every step, which is what a quicksort with no
    // fallback goes quadratic on.
    for (isize const n : {100, 1000, 5000})
    {
        SECTION("n = {}", n)
        {
            auto adv = adversary::create_for(n);

            auto values = cc::vector<isize>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                values[i] = i;

            cc::sort(values, adv);

            // the answers were consistent throughout, so the result must be ordered by the final values
            for (isize i = 1; i < n; ++i)
                CHECK(adv.values[values[i - 1]] <= adv.values[values[i]]);

            // n log2(n) with generous slack: a run that went quadratic would be orders of magnitude over
            CHECK(adv.comparison_count < isize(20) * n * isize(cc::bit_width(u64(n))));
        }
    }
}

#if CC_ASSERT_ENABLED
TEST("sort - a comparator that is not a strict weak ordering asserts")
{
    // everything compares less than everything, which runs the unguarded partition scans off the range
    auto always_less = [](i32, i32) { return true; };

    auto values = cc::vector<i32>::create_defaulted(2000);
    for (isize i = 0; i < 2000; ++i)
        values[i] = i32(i % 97);

    CHECK_ASSERTS(cc::sort(values, always_less));
}
#endif

TEST("sort - only swaps, never copies an element out of the range")
{
    cc::random rng(12);
    auto values = cc::vector<copy_counted>::create_defaulted(1000);
    for (isize i = 0; i < 1000; ++i)
        values[i] = copy_counted(rng.uniform(0, 10000));

    copy_counted::copies = 0;
    cc::sort(values);

    CHECK(copy_counted::copies == 0);
    CHECK(cc::is_sorted(values));
}

TEST("sort - a hand-written index_swap_range adapter")
{
    // a structure-of-arrays view: reads one array, permutes three
    struct soa_range
    {
        cc::vector<i32>* keys;
        cc::vector<f32>* xs;
        cc::vector<f32>* ys;

        i32 element_get(isize i) const { return (*keys)[i]; }
        void element_swap(isize a, isize b) const
        {
            cc::swap((*keys)[a], (*keys)[b]);
            cc::swap((*xs)[a], (*xs)[b]);
            cc::swap((*ys)[a], (*ys)[b]);
        }
    };
    static_assert(cc::index_swap_range<soa_range>);

    isize const n = 300;
    auto keys = cc::vector<i32>::create_defaulted(n);
    auto xs = cc::vector<f32>::create_defaulted(n);
    auto ys = cc::vector<f32>::create_defaulted(n);

    cc::random rng(13);
    for (isize i = 0; i < n; ++i)
    {
        keys[i] = rng.uniform(0, 1000);
        xs[i] = f32(keys[i]) * 2.0f;
        ys[i] = f32(keys[i]) * 3.0f;
    }

    auto const range = soa_range{.keys = &keys, .xs = &xs, .ys = &ys};
    cc::sort_ex(0, n, range, cc::default_less{}, cc::constant_function<true>{});

    REQUIRE(cc::is_sorted(keys));
    for (isize i = 0; i < n; ++i)
    {
        CHECK(xs[i] == f32(keys[i]) * 2.0f);
        CHECK(ys[i] == f32(keys[i]) * 3.0f);
    }
}

TEST("sort - select prunes whole subranges")
{
    cc::random rng(14);
    isize const n = 500;
    auto values = make_pattern(pattern::random, n, rng);
    auto const original = values;

    // only the subranges covering index 0 are sorted, so the smallest element must land there
    auto smallest = values[0];
    for (isize i = 1; i < n; ++i)
        smallest = cc::min(smallest, values[i]);

    cc::sort_ex(0, n, cc::as_index_swap_range(values), cc::default_less{}, [](isize start, isize) { return start == 0; });

    CHECK(values[0] == smallest);
    CHECK(is_permutation_of(original, values));
}

TEST("sort - element types that take the non-branchless path")
{
    // a wide or non-trivially-copyable element switches off block partitioning and holds the pivot by
    // reference instead of copying it, which is a second partition implementation to cover
    cc::random rng(15);

    SECTION("wider than the pivot-copy threshold")
    {
        isize const n = 2000;
        auto values = cc::vector<wide>::create_defaulted(n);
        for (isize i = 0; i < n; ++i)
            values[i].key = rng.uniform(0, 500); // duplicates, so the equal-element partition runs too

        cc::sort(values);
        CHECK(cc::is_sorted(values));
    }

    SECTION("not trivially copyable")
    {
        isize const n = 2000;
        auto values = cc::vector<cc::string>::create_defaulted(n);
        for (isize i = 0; i < n; ++i)
            values[i] = cc::to_string(rng.uniform(0, 100000));

        auto expected = values;
        cc::sort(values);

        REQUIRE(cc::is_sorted(values));

        // the same multiset, checked without leaning on the sort under test
        cc::map<cc::string, isize> counts;
        for (isize i = 0; i < n; ++i)
            counts[expected[i]] += 1;
        for (isize i = 0; i < n; ++i)
            counts[values[i]] -= 1;
        for (auto const [key, count] : counts)
            CHECK(count == 0);
    }
}

TEST("sort - the heapsort fallback sorts every pattern")
{
    // reached only when a run keeps splitting badly, so it is driven directly here rather than by luck
    cc::random rng(16);

    for (auto const p : all_patterns)
    {
        SECTION("{}", to_name(p))
        {
            auto const original = make_pattern(p, 500, rng);
            auto values = original;

            auto compare = cc::default_less{};
            cc::impl::sort_heapsort(0, 500, cc::as_index_swap_range(values), compare);

            CHECK(cc::is_sorted(values));
            CHECK(is_permutation_of(original, values));
        }
    }
}

TEST("sort - sorts at compile time")
{
    static constexpr auto sorted_at_compile_time = []
    {
        auto values = cc::fixed_array<int, 7>{5, 3, 9, 1, 7, 2, 8};
        cc::sort(values);
        return values;
    }();

    static_assert(sorted_at_compile_time[0] == 1);
    static_assert(sorted_at_compile_time[6] == 9);
    static_assert(cc::is_sorted(sorted_at_compile_time));

    CHECK(sorted_at_compile_time[3] == 5);
}

TEST("sort_stable - equal elements keep their original order")
{
    cc::random rng(17);

    for (isize const n : {isize(1), isize(17), isize(1000), isize(20000)})
    {
        SECTION("n = {}", n)
        {
            // the key is deliberately narrow, so almost every element ties with many others
            struct sample
            {
                i32 key = 0;
                i32 arrival = 0;
            };

            auto values = cc::vector<sample>::create_defaulted(n);
            for (isize i = 0; i < n; ++i)
                values[i] = {.key = rng.uniform(0, 9), .arrival = i32(i)};

            cc::sort_stable(values, [](sample const& a, sample const& b) { return a.key < b.key; });

            CHECK(isize(values.size()) == n);
            for (isize i = 1; i < n; ++i)
            {
                CHECK(values[i - 1].key <= values[i].key);

                // within one key the arrival order must be untouched, which is the whole claim
                if (values[i - 1].key == values[i].key)
                    CHECK(values[i - 1].arrival < values[i].arrival);
            }
        }
    }
}

TEST("sort_stable - agrees with sort on the value sequence")
{
    cc::random rng(18);

    for (auto const p : all_patterns)
    {
        SECTION("{}", to_name(p))
        {
            auto const original = make_pattern(p, 700, rng);

            auto stable = original;
            cc::sort_stable(stable);

            auto unstable = original;
            cc::sort(unstable);

            // stability picks WHICH equal element lands where, never which value does
            for (isize i = 0; i < 700; ++i)
                CHECK(stable[i] == unstable[i]);
        }
    }
}

TEST("sort_stable_by - a key projection, ties still in arrival order")
{
    struct sample
    {
        i32 key = 0;
        i32 arrival = 0;
    };

    isize const n = 500;
    cc::random rng(19);

    auto values = cc::vector<sample>::create_defaulted(n);
    for (isize i = 0; i < n; ++i)
        values[i] = {.key = rng.uniform(0, 4), .arrival = i32(i)};

    cc::sort_stable_by(values, &sample::key);

    for (isize i = 1; i < n; ++i)
    {
        CHECK(values[i - 1].key <= values[i].key);
        if (values[i - 1].key == values[i].key)
            CHECK(values[i - 1].arrival < values[i].arrival);
    }
}

TEST("sort_stable - descending, and an empty range")
{
    auto values = cc::vector<i32>{3, 1, 2};
    cc::sort_stable(values, cc::default_greater{});
    CHECK(values[0] == 3);
    CHECK(values[1] == 2);
    CHECK(values[2] == 1);

    auto empty = cc::vector<i32>();
    cc::sort_stable(empty);
    CHECK(empty.empty());
}
