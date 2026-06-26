// Standalone byte-hash throughput benchmark (not strings).
//
// Isolates the cost of the byte-range hash itself: our wrappers (cc::make_hash_of_bytes 64-bit,
// cc::hash128::create 128-bit) against the raw xxHash entry points they call (XXH3_64bits_withSeed /
// XXH3_128bits_withSeed). The wrapper and raw columns move together in shape across lengths and configs; they
// differ only by a fixed per-call cost (the wrappers live in clean-core's own TU and, without LTO, cannot be
// inlined into the caller), which is large relative to a 2 ns hash for tiny keys but vanishes past a few
// dozen bytes. The point of interest is how the vendored xxHash build itself performs.
//
// Motivation: in the default RelWithDebInfo dev build, clang-cl compiled with /Ob1 (inline only functions
// marked inline), which kept xxHash's short/mid-key path (<= XXH3_MIDSIZE_MAX = 240 bytes, a chain of plain
// `static` helpers) ~5-11x slower than Release /Ob2. RelWithDebInfo is now /Ob2 project-wide (root
// CMakeLists), and the wrappers carry CC_PURE; this benchmark is how that was measured and verified. See
// libs/base/clean-core/docs/benchmarks/hash-benchmark.md.
//
// This benchmark links xxhash directly (normally private to clean-core) so it can call the raw entry points.
// `as_bytes` is force-inlined so the only difference between the wrapper and raw columns is the wrapper's own
// out-of-line call, not benchmark plumbing (under /Ob1 an unmarked helper would itself stay out-of-line).
//
// Manual test (nx::config::manual): prints only, no CHECK. Run e.g.
//   uv run dev.py test "bench-hash" --target clean-core-test --preset release-clang --timeout 0

#include "bench_util.hh"

#include <clean-core/common/hash.hh>
#include <clean-core/common/hash128.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <nexus/test.hh>
#include <xxhash.h>

#include <cstdio>

using namespace cc::primitive_defines;

namespace
{
CC_FORCE_INLINE cc::span<cc::byte const> as_bytes(char const* p, size_t n)
{
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(p), isize(n));
}

void run()
{
    auto const lengths = bench::hash_lengths();
    cc::random rng(0xABCDEFu);

    std::printf("\n=== byte hash throughput (GB/s) — distinct keys ===\n");
    std::printf("%8s %12s %12s %12s %12s\n", "length", "hob64", "hash128", "xxh64", "xxh128");
    std::printf("%8s %12s %12s %12s %12s\n", "------", "-----", "-------", "-----", "------");

    for (isize const length : lengths)
    {
        isize count = (8 * 1024 * 1024) / length;
        count = cc::clamp(count, isize(64), isize(200000));

        cc::vector<char> buffer;
        buffer.resize_to_uninitialized(count * length);
        for (isize i = 0; i < buffer.size(); ++i)
            buffer[i] = char(rng.uniform(0, 255));

        double const bytes_per_pass = double(count) * double(length);

        auto const gbps = [&](auto hasher)
        {
            return bench::measure_units_per_sec(bytes_per_pass,
                                                [&]
                                                {
                                                    u64 acc = 0;
                                                    for (isize i = 0; i < count; ++i)
                                                        acc ^= hasher(buffer.data() + i * length, size_t(length));
                                                    return acc;
                                                })
                 / 1e9;
        };

        double const g_hob = gbps([](char const* p, size_t n) { return cc::make_hash_of_bytes(as_bytes(p, n), 0); });
        double const g_h128 = gbps(
            [](char const* p, size_t n)
            {
                auto const h = cc::hash128::create(as_bytes(p, n), 0);
                return h.low ^ h.high;
            });
        double const g_x64 = gbps([](char const* p, size_t n) { return u64(XXH3_64bits_withSeed(p, n, 0)); });
        double const g_x128 = gbps(
            [](char const* p, size_t n)
            {
                auto const h = XXH3_128bits_withSeed(p, n, 0);
                return u64(h.low64 ^ h.high64);
            });

        std::printf("%8lld %12.2f %12.2f %12.2f %12.2f\n", (long long)length, g_hob, g_h128, g_x64, g_x128);
    }
    std::fflush(stdout);
}
} // namespace

TEST("bench-hash (xxh3 64/128, raw vs wrapper)", nx::config::manual)
{
    run();
}
