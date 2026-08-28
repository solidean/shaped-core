// cc::sort against std::sort, over the input shapes and element types a comparison sort behaves differently on.
//
// This is the measurement that decides whether the swap-only formulation costs anything: cc::sort may never park an
// element in a temporary, so its small sort shifts by swapping where a move-based one moves once per element.
// It is also what would justify ever adding a move-based fallback for the small sort.
//
// The BENCHMARKs are the comparison; the PGO_BENCHMARK at the bottom records two stable points for the PGO report.
// Run them with
//   uv run dev.py benchmark "bench-sort"
//
// docs/guides/benchmarking.md is the workflow.

#include "../algorithm/sort-test-types.hh"

#include <clean-core/algorithm/permutation.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/algorithm/sort_async.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/bench/run.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>

#include <algorithm>
#include <cstdio>

using namespace cc::primitive_defines;
using namespace sort_test;

namespace
{
/// A payload wide enough that the pivot is held by reference and block partitioning is off.
struct wide
{
    i64 key = 0;
    i64 padding[7] = {};

    friend bool operator<(wide const& a, wide const& b) { return a.key < b.key; }
};

template <class T>
cc::vector<T> convert_from(cc::vector<i32> const& source);

template <>
cc::vector<i32> convert_from<i32>(cc::vector<i32> const& source)
{
    return source;
}

template <>
cc::vector<u64> convert_from<u64>(cc::vector<i32> const& source)
{
    auto out = cc::vector<u64>::create_defaulted(isize(source.size()));
    for (isize i = 0; i < isize(source.size()); ++i)
        out[i] = u64(i64(source[i]) + 2000000);
    return out;
}

template <>
cc::vector<wide> convert_from<wide>(cc::vector<i32> const& source)
{
    auto out = cc::vector<wide>::create_defaulted(isize(source.size()));
    for (isize i = 0; i < isize(source.size()); ++i)
        out[i].key = source[i];
    return out;
}

template <>
cc::vector<cc::string> convert_from<cc::string>(cc::vector<i32> const& source)
{
    auto out = cc::vector<cc::string>::create_defaulted(isize(source.size()));
    for (isize i = 0; i < isize(source.size()); ++i)
        out[i] = cc::to_string(source[i] + 2000000);
    return out;
}

i64 key_of(i32 v)
{
    return v;
}
i64 key_of(u64 v)
{
    return i64(v);
}
i64 key_of(wide const& v)
{
    return v.key;
}
i64 key_of(cc::string const& v)
{
    return isize(v.size()) + (v.empty() ? 0 : v[0]);
}

/// Reads a handful of elements so the sort cannot be optimized away, without costing a full pass.
template <class T>
u64 checksum_of(cc::vector<T> const& values)
{
    isize const n = isize(values.size());
    i64 sum = n;
    for (isize k = 0; k < 8; ++k)
        if (n > 0)
            sum += key_of(values[(k * n) / 8]);
    return u64(sum);
}

/// One measured sort, with the cost of restoring the unsorted input excluded rather than subtracted.
///
/// Every iteration must start from the same input, so the refill is unavoidable — and `pause`/`resume` takes it out of
/// the measurement directly.
/// The previous formulation measured sort-plus-refill and refill separately and subtracted the two medians, which is
/// not a thing a median supports: the difference of two medians has no interval, so the result could not say how much
/// of itself to believe.
template <class T, class SortF>
nx::bench::result sort_rate(cc::string_view name, cc::vector<T> const& original, SortF&& sort_fn)
{
    auto const n = cc::max(isize(original.size()), isize(1));

    // Several arrays per iteration where one is too small to measure.
    //
    // A pause/resume pair costs about 14 ns, so a 16-element sort — which is 20 to 80 ns — would be a third clock and
    // two thirds sort, and the harness says so.
    // Sorting a block of them per iteration amortizes the pair, and the per-item rate is unchanged by construction.
    auto const copies = cc::max(isize(1), isize(4096) / n);

    auto scratch = cc::vector<cc::vector<T>>::create_defaulted(copies);
    for (auto& s : scratch)
        s = cc::vector<T>::create_defaulted(n);

    return nx::bench::run(name, {.no_baseline = true},
                          [&](nx::bench::iteration& it)
                          {
                              it.pause();
                              for (auto& s : scratch)
                                  for (isize i = 0; i < n; ++i)
                                      s[i] = original[i];
                              it.resume();

                              for (auto& s : scratch)
                              {
                                  sort_fn(s);
                                  nx::bench::sink(checksum_of(s));
                              }
                              it.items(n * copies);
                          });
}

template <class T>
void run_type(char const* type_name, cc::random& rng)
{
    for (auto const p : all_patterns)
    {
        for (isize const n : {isize(16), isize(1000), isize(1000000)})
        {
            auto const source = make_pattern(p, n, rng);
            auto const original = convert_from<T>(source);

            // Two loops per point, so cc::sort and std::sort are compared against each other by the harness rather
            // than by a ratio column this file used to compute itself.
            sort_rate(cc::format("{} {} n={} cc::sort", type_name, to_name(p), n), original,
                      [](cc::vector<T>& v) { cc::sort(v); });
            sort_rate(cc::format("{} {} n={} std::sort", type_name, to_name(p), n), original,
                      [](cc::vector<T>& v) { std::sort(v.begin(), v.end()); });
        }
    }
}
} // namespace

// One BENCHMARK per element type rather than one for all four: a comparison table is only worth reading when its rows
// are comparable, and a u64 sort against a cc::string sort is not a comparison anyone wants.
BENCHMARK("bench-sort - i32")
{
    cc::random rng(1);
    run_type<i32>("i32", rng);
}

BENCHMARK("bench-sort - u64")
{
    cc::random rng(1);
    run_type<u64>("u64", rng);
}

BENCHMARK("bench-sort - wide (64 B)")
{
    cc::random rng(1);
    run_type<wide>("wide", rng);
}

BENCHMARK("bench-sort - cc::string")
{
    cc::random rng(1);
    run_type<cc::string>("string", rng);
}

BENCHMARK("bench-sort - sort_multi vs sort_indices then permute")
{
    cc::random rng(2);
    isize const n = 1000000;

    auto const keys_source = make_pattern(pattern::random, n, rng);
    auto const payload_source = convert_from<wide>(keys_source);

    auto keys = keys_source;
    auto payload = payload_source;
    auto order = cc::vector<i32>::create_defaulted(n);

    nx::bench::run("sort_multi",
                   [&](nx::bench::iteration& it)
                   {
                       it.pause();
                       keys = keys_source;
                       payload = payload_source;
                       it.resume();

                       cc::sort_multi(cc::default_less{}, keys, payload);
                       nx::bench::sink(checksum_of(keys));
                       it.items(n);
                   });

    nx::bench::run("sort_indices (permutation not applied)",
                   [&](nx::bench::iteration& it)
                   {
                       it.pause();
                       for (isize i = 0; i < n; ++i)
                           order[i] = i32(i);
                       it.resume();

                       cc::sort_indices(order, keys_source);
                       nx::bench::sink(checksum_of(order));
                       it.items(n);
                   });

    // The comparable number: sort the indices, then actually move the payload, which is what sort_multi did.
    nx::bench::run("sort_indices + apply_permutation",
                   [&](nx::bench::iteration& it)
                   {
                       it.pause();
                       payload = payload_source;
                       for (isize i = 0; i < n; ++i)
                           order[i] = i32(i);
                       it.resume();

                       cc::sort_indices(order, keys_source);
                       cc::apply_permutation(payload, order);
                       nx::bench::sink(checksum_of(payload));
                       it.items(n);
                   });
}

#if CC_HAS_THREADS
// A worker sweep is meaningless without threads: the pool keeps its API but drives inline, so every row would be
// the serial number with scheduling overhead on top.
// The correctness of that fallback is sort-async-test.cc's.
BENCHMARK("bench-sort - sort_async worker sweep")
{
    cc::random rng(4);
    int const max_workers = cc::async_thread_pool::default_worker_count();

    for (auto const p : {pattern::random, pattern::sorted, pattern::few_values})
    {
        for (isize const n : {isize(1) << 20, isize(1) << 22})
        {
            auto const original = make_pattern(p, n, rng);

            // The serial baseline is re-measured per point rather than taken once: it doubles as a throttling canary,
            // since a machine that clocked down mid-sweep moves every row of that point together.
            sort_rate(cc::format("{} n={} cc::sort", to_name(p), n), original, [](cc::vector<i32>& v) { cc::sort(v); });

            for (int const workers : {1, 2, 4, max_workers})
            {
                if (workers > max_workers)
                    continue;

                // Constructed outside the measured body: spawning threads is ~100 us and would land inside it.
                cc::async_thread_pool pool(workers);
                sort_rate(cc::format("{} n={} async x{}", to_name(p), n, workers), original,
                          [&](cc::vector<i32>& v)
                          {
                              auto const sorted = cc::sort_async(v);
                              pool.participate_until_ready(*sorted);
                          });
            }
        }
    }
}

// What justifies the shipped sort_async_default_cutoff, which is otherwise a guess.
BENCHMARK("bench-sort - sort_async cutoff sweep")
{
    cc::random rng(4);
    int const max_workers = cc::async_thread_pool::default_worker_count();

    auto const original = make_pattern(pattern::random, isize(1) << 22, rng);
    sort_rate("cc::sort", original, [](cc::vector<i32>& v) { cc::sort(v); });

    cc::async_thread_pool pool(max_workers);
    for (isize const cutoff : {isize(1024), isize(4096), isize(16384), isize(65536), isize(262144)})
    {
        sort_rate(cc::format("async cutoff={}", cutoff), original,
                  [&](cc::vector<i32>& v)
                  {
                      auto const sorted = cc::sort_async_ex(0, isize(v.size()), cc::as_index_swap_range(v),
                                                            cc::default_less{}, cutoff);
                      pool.participate_until_ready(*sorted);
                  });
    }
}
#endif

PGO_BENCHMARK("bench-sort - throughput")
{
    cc::random rng(3);

    for (isize const n : {isize(1000), isize(1000000)})
    {
        auto const original = make_pattern(pattern::random, n, rng);
        auto const name = cc::format("cc::sort i32 random@{}", n);

        // nx::bench::run outside a BENCHMARK reports to nobody and hands the result back, which is exactly what a PGO
        // benchmark wants: one number, recorded through nx::pgo.
        auto const r = sort_rate(name, original, [](cc::vector<i32>& v) { cc::sort(v); });
        nx::pgo::report_elements_per_sec(name, r.items_per_second);
    }
}
