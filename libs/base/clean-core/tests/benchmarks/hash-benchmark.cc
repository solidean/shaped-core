// Standalone byte-hash throughput benchmark (not strings).
//
// Isolates the cost of the byte-range hash itself: our wrappers (cc::make_hash_of_bytes 64-bit, cc::hash128::create 128-bit)
// against the raw xxHash entry points they call (XXH3_64bits_withSeed / XXH3_128bits_withSeed).
//
// cc::blake3 rides along in the same table, next to the XXH3 columns rather than in a benchmark of its own.
// That comparison is the point: BLAKE3 is the cryptographic hash content addressing needs, it costs several times
// what XXH3 does, and libs/data/versioned-document/docs/decisions.md attaches a standing reservation to exactly
// that ratio.
// Measuring both here is what keeps the ratio a number in the repo rather than a claim in a document.
// The wrapper and raw columns move together in shape across lengths and configs, differing only by a fixed per-call cost:
// the wrappers live in clean-core's own TU and cannot be inlined into the caller without LTO.
// That cost is large relative to a 2 ns hash for tiny keys, and vanishes past a few dozen bytes.
// The point of interest is how the vendored xxHash build itself performs.
//
// Motivation: in the default RelWithDebInfo dev build, clang-cl compiled with /Ob1 (inline only functions marked inline).
// That kept xxHash's short/mid-key path — <= XXH3_MIDSIZE_MAX = 240 bytes, a chain of plain `static` helpers — some 5-11x slower than Release /Ob2.
// RelWithDebInfo is now /Ob2 project-wide (root CMakeLists), and the wrappers carry CC_PURE.
// This benchmark is how that was measured and verified; the write-up is ../../docs/benchmarks/hash-benchmark.md.
//
// This benchmark links xxhash and blake3 directly (normally private to clean-core) so it can call the raw entry points.
// `as_bytes` is force-inlined so the only difference between the wrapper and raw columns is the wrapper's own out-of-line call, not benchmark plumbing.
// Under /Ob1 an unmarked helper would itself stay out-of-line.
//
// GUIDE_BENCHMARK runs only the three representative lengths (≈8 B, ≈256 B and ≈64 KiB) and records them via nx::guide for the PGO speedup report.
// 256 B is there for BLAKE3: an op payload is a few hundred bytes, so that is the size its cost is actually argued about at.
// The full length table comes from the manual sweep at the bottom of this file.
// Run e.g.
//   uv run dev.py test "bench-hash" --target clean-core-test --preset release-clang --timeout 0

#include "bench_util.hh"

#include <blake3.h>
#include <clean-core/bytes/blake3.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <nexus/guide.hh>
#include <nexus/test.hh>
#include <xxhash.h>

#include <cstdio>

using namespace cc::primitive_defines;

namespace
{
CC_FORCE_INLINE cc::span<byte const> as_bytes(char const* p, size_t n)
{
    return cc::span<char const>(p, isize(n)).as_bytes();
}

// Sweeps `lengths`, printing one throughput row each.
// When `record`, the points nearest 8 B and 64 KiB are also reported as guide metrics.
// Pass the representative-only lengths for a fast guide benchmark, or the full sweep (record=false) for the human analysis table.
void run(cc::span<isize const> lengths, bool record)
{
    cc::random rng(0xABCDEFu);

    std::printf("\n=== byte hash throughput (GB/s) — distinct keys ===\n");
    std::printf("%8s %12s %12s %12s %12s %12s %12s\n", "length", "hob64", "hash128", "xxh64", "xxh128", "blake3",
                "b3raw");
    std::printf("%8s %12s %12s %12s %12s %12s %12s\n", "------", "-----", "-------", "-----", "------", "------",
                "-----");

    // Track the sweep point nearest each target length so the recorded metrics stay stable regardless of the exact sweep membership.
    // Values are filled in below and reported after the loop.
    struct rep_point
    {
        isize target;
        char const* label;
        isize best_len = -1;
        double hob = 0, h128 = 0, x64 = 0, x128 = 0, b3 = 0, b3raw = 0;
    };
    rep_point reps[] = {{8, "8B"}, {256, "256B"}, {64 * 1024, "64KiB"}};
    auto const dist = [](isize a, isize b) { return a > b ? a - b : b - a; };

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

        double const g_b3 = gbps(
            [](char const* p, size_t n)
            {
                auto const h = cc::blake3::create(as_bytes(p, n));
                return h.l0 ^ h.l1 ^ h.l2 ^ h.l3;
            });
        double const g_b3raw = gbps(
            [](char const* p, size_t n)
            {
                blake3_hasher hasher;
                blake3_hasher_init(&hasher);
                blake3_hasher_update(&hasher, p, n);

                u64 digest = 0;
                blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t*>(&digest), sizeof(digest));
                return digest;
            });

        std::printf("%8lld %12.2f %12.2f %12.2f %12.2f %12.2f %12.2f\n", (long long)length, g_hob, g_h128, g_x64,
                    g_x128, g_b3, g_b3raw);

        for (auto& r : reps)
            if (r.best_len < 0 || dist(length, r.target) < dist(r.best_len, r.target))
                r = {r.target, r.label, length, g_hob, g_h128, g_x64, g_x128, g_b3, g_b3raw};
    }
    std::fflush(stdout);

    if (record)
        for (auto const& r : reps)
        {
            nx::guide::report_raw(cc::string("hob64@") + r.label, r.hob, "GB/s", true);
            nx::guide::report_raw(cc::string("hash128@") + r.label, r.h128, "GB/s", true);
            nx::guide::report_raw(cc::string("xxh64@") + r.label, r.x64, "GB/s", true);
            nx::guide::report_raw(cc::string("xxh128@") + r.label, r.x128, "GB/s", true);
            nx::guide::report_raw(cc::string("blake3@") + r.label, r.b3, "GB/s", true);
            nx::guide::report_raw(cc::string("b3raw@") + r.label, r.b3raw, "GB/s", true);
        }
}

// The representative lengths the guide benchmark sweeps: one short key (8 B), one op-sized (256 B) and one long (64 KiB), matching the points reported as metrics.
// Far faster than the full sweep, while still exercising every hash code path.
constexpr isize guide_lengths[] = {8, 256, 64 * 1024};
} // namespace

// Lean guide benchmark: just the representative lengths, recorded for the PGO speedup report.
GUIDE_BENCHMARK("bench-hash (xxh3 64/128, raw vs wrapper)")
{
    run(guide_lengths, /*record*/ true);
}

// Full human-facing sweep (manual): the complete length table the docs analyze.
// Run by exact name.
TEST("bench-hash (xxh3 64/128, raw vs wrapper, full sweep)", nx::config::manual)
{
    run(bench::hash_lengths(), /*record*/ false);
}
