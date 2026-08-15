// cc::sort against std::sort, over the input shapes and element types a comparison sort behaves differently on.
//
// This is the measurement that decides whether the swap-only formulation costs anything: cc::sort may never park an
// element in a temporary, so its small sort shifts by swapping where a move-based one moves once per element.
// It is also what would justify ever adding a move-based fallback for the small sort.
//
// The manual sweep is the comparison; the GUIDE_BENCHMARK at the bottom records two stable points for the PGO report.
// Run the sweep with
//   uv run dev.py test "bench-sort - cc::sort vs std::sort" --preset release-clang --timeout 0

#include "../algorithm/sort-test-types.hh"
#include "bench_util.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/to_string.hh>
#include <nexus/guide.hh>
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

/// Elements sorted per second, with the cost of restoring the unsorted input measured out.
/// Every pass must start from the same input, so the refill is unavoidable and is subtracted rather than ignored.
template <class T, class SortF>
double sort_rate(cc::vector<T> const& original, SortF&& sort_fn)
{
    auto const n = double(original.size());
    auto scratch = cc::vector<T>::create_defaulted(isize(original.size()));

    auto const refill = [&]
    {
        for (isize i = 0; i < isize(original.size()); ++i)
            scratch[i] = original[i];
    };

    double const rate_total = bench::median_units_per_sec(n,
                                                          [&]
                                                          {
                                                              refill();
                                                              sort_fn(scratch);
                                                              return checksum_of(scratch);
                                                          });
    double const rate_refill = bench::median_units_per_sec(n,
                                                           [&]
                                                           {
                                                               refill();
                                                               return checksum_of(scratch);
                                                           });

    double const seconds = (n / rate_total) - (n / rate_refill);
    if (seconds <= 0)
        return 0; // the refill dominated, so this point carries no signal
    return n / seconds;
}

template <class T>
void run_type(char const* type_name, cc::random& rng)
{
    std::printf("\n%s\n", type_name);
    std::printf("  %-16s %10s %14s %14s %9s\n", "pattern", "n", "cc M/s", "std M/s", "ratio");

    for (auto const p : all_patterns)
    {
        for (isize const n : {isize(16), isize(1000), isize(1000000)})
        {
            auto const source = make_pattern(p, n, rng);
            auto const original = convert_from<T>(source);

            double const cc_rate = sort_rate(original, [](cc::vector<T>& v) { cc::sort(v); });
            double const std_rate = sort_rate(original, [](cc::vector<T>& v) { std::sort(v.begin(), v.end()); });

            std::printf("  %-16s %10lld %14.2f %14.2f %9.2f\n", to_name(p), static_cast<long long>(n), cc_rate / 1e6,
                        std_rate / 1e6, std_rate > 0 ? cc_rate / std_rate : 0.0);
        }
    }
}
} // namespace

TEST("bench-sort - cc::sort vs std::sort", nx::config::manual)
{
    cc::random rng(1);

    run_type<i32>("i32", rng);
    run_type<u64>("u64", rng);
    run_type<wide>("wide (64 B)", rng);
    run_type<cc::string>("cc::string", rng);
}

TEST("bench-sort - sort_multi vs sort_indices then permute", nx::config::manual)
{
    cc::random rng(2);
    isize const n = 1000000;

    auto const keys_source = make_pattern(pattern::random, n, rng);
    auto const payload_source = convert_from<wide>(keys_source);

    std::printf("\nkeys + one 64 B payload, n = %lld\n", static_cast<long long>(n));

    auto keys = keys_source;
    auto payload = payload_source;
    double const multi_rate = bench::median_units_per_sec(double(n),
                                                          [&]
                                                          {
                                                              keys = keys_source;
                                                              payload = payload_source;
                                                              cc::sort_multi(cc::default_less{}, keys, payload);
                                                              return checksum_of(keys);
                                                          });

    auto order = cc::vector<i32>::create_defaulted(n);
    double const indices_rate = bench::median_units_per_sec(double(n),
                                                            [&]
                                                            {
                                                                for (isize i = 0; i < n; ++i)
                                                                    order[i] = i32(i);
                                                                cc::sort_indices(order, keys_source);
                                                                return checksum_of(order);
                                                            });

    std::printf("  sort_multi     %10.2f M/s\n", multi_rate / 1e6);
    std::printf("  sort_indices   %10.2f M/s  (permutation not applied)\n", indices_rate / 1e6);
}

GUIDE_BENCHMARK("bench-sort - throughput")
{
    cc::random rng(3);

    for (isize const n : {isize(1000), isize(1000000)})
    {
        auto const original = make_pattern(pattern::random, n, rng);
        double const rate = sort_rate(original, [](cc::vector<i32>& v) { cc::sort(v); });

        auto const name = cc::format("cc::sort i32 random@{}", n);
        nx::guide::report_elements_per_sec(name, rate);
    }
}
