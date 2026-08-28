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
// PGO_BENCHMARK runs only the three representative lengths (~8 B, ~256 B and ~64 KiB) and records them via nx::pgo for the PGO speedup report.
// 256 B is there for BLAKE3: an op payload is a few hundred bytes, so that is the size its cost is actually argued about at.
// The full length table comes from the sweep BENCHMARK below it.
// Run e.g.
//   uv run dev.py benchmark "bench-hash"

#include <blake3.h>
#include <clean-core/bytes/blake3.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>
#include <xxhash.h>

using namespace cc::primitive_defines;

namespace
{
CC_FORCE_INLINE cc::span<byte const> as_bytes(char const* p, size_t n)
{
    return cc::span<char const>(p, isize(n)).as_bytes();
}

/// The length sweep the docs analyze: 1..32 (every length), then +8 up to 64, then *1.5 up to ~100k.
///
/// Benchmark DATA rather than harness, so it lives next to the only benchmark that sweeps it.
cc::vector<isize> hash_lengths()
{
    cc::vector<isize> lengths;
    for (isize l = 1; l <= 32; ++l)
        lengths.push_back(l);
    for (isize l = 40; l <= 64; l += 8)
        lengths.push_back(l);
    for (isize l = 64;;)
    {
        isize next = isize(double(l) * 1.5);
        if (next <= l)
            next = l + 1;
        if (next > 100000)
            break;
        lengths.push_back(next);
        l = next;
    }
    return lengths;
}

/// One buffer of `count` distinct keys of `length` bytes, and the loop that hashes it.
///
/// Every hasher is one loop, so the six of them become one comparison table per length — which is the shape the docs
/// read, without this file printing a table of its own.
struct length_point
{
    isize length = 0;
    isize count = 0;
    cc::vector<char> buffer;

    explicit length_point(isize len, cc::random& rng) : length(len)
    {
        count = cc::clamp((8 * 1024 * 1024) / len, isize(64), isize(200000));
        buffer.resize_to_uninitialized(count * length);
        for (isize i = 0; i < buffer.size(); ++i)
            buffer[i] = char(rng.uniform(0, 255));
    }

    /// Hash every key once, recording the bytes covered so the harness derives B/s itself.
    template <class Hasher>
    nx::bench::result measure(cc::string_view name, nx::bench::run_config const& cfg, Hasher hasher) const
    {
        return nx::bench::run(name, cfg,
                              [&](nx::bench::iteration& it)
                              {
                                  u64 acc = 0;
                                  for (isize i = 0; i < count; ++i)
                                      acc ^= hasher(buffer.data() + i * length, size_t(length));
                                  nx::bench::sink(acc);

                                  it.items(count); // hashes
                                  it.record("bytes", cc::rec::unit_bytes, f64(count) * f64(length));
                              });
    }
};

// The six hashers, in the order the docs' table reads.
u64 hash_hob64(char const* p, size_t n)
{
    return cc::make_hash_of_bytes(as_bytes(p, n), 0);
}

u64 hash_h128(char const* p, size_t n)
{
    auto const h = cc::hash128::create(as_bytes(p, n), 0);
    return h.low ^ h.high;
}

u64 hash_x64(char const* p, size_t n)
{
    return u64(XXH3_64bits_withSeed(p, n, 0));
}

u64 hash_x128(char const* p, size_t n)
{
    auto const h = XXH3_128bits_withSeed(p, n, 0);
    return u64(h.low64 ^ h.high64);
}

u64 hash_b3(char const* p, size_t n)
{
    auto const h = cc::blake3::create(as_bytes(p, n));
    return h.l0 ^ h.l1 ^ h.l2 ^ h.l3;
}

u64 hash_b3raw(char const* p, size_t n)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, p, n);

    u64 digest = 0;
    blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t*>(&digest), sizeof(digest));
    return digest;
}

/// Every hasher at one length.
/// `suffix` distinguishes the loops when several lengths land in the same benchmark.
void run_length(length_point const& point, nx::bench::run_config const& cfg, cc::string_view suffix)
{
    point.measure(cc::format("hob64{}", suffix), cfg, hash_hob64);
    point.measure(cc::format("hash128{}", suffix), cfg, hash_h128);
    point.measure(cc::format("xxh64{}", suffix), cfg, hash_x64);
    point.measure(cc::format("xxh128{}", suffix), cfg, hash_x128);
    point.measure(cc::format("blake3{}", suffix), cfg, hash_b3);
    point.measure(cc::format("b3raw{}", suffix), cfg, hash_b3raw);
}

/// The representative lengths: one short key (8 B), one op-sized (256 B) and one long (64 KiB).
/// Far faster than the full sweep, while still exercising every hash code path.
constexpr isize representative_lengths[] = {8, 256, 64 * 1024};

/// The byte throughput off a measured loop, which is what the PGO report tracks.
///
/// Reported in the harness's own unit rather than converted: a PGO series is compared within one `dev.py pgo measure`
/// invocation — baseline against pgo-use — so there is no earlier number to stay compatible with.
double bytes_per_second_of(nx::bench::result const& r)
{
    auto const* const bytes = r.find_quantity("bytes");
    return bytes != nullptr ? bytes->per_second : 0.0;
}
} // namespace

// The three representative lengths, six hashers each.
BENCHMARK("bench-hash - representative lengths")
{
    cc::random rng(0xABCDEFu);
    for (auto const length : representative_lengths)
    {
        auto const point = length_point(length, rng);
        // A sweep across lengths: comparing 8 B against 64 KiB is a statement about the lengths, not the hashers.
        run_length(point, {.no_baseline = true}, cc::format(" @{}", length));
    }
}

// The complete length table the docs analyze.
// A lower min_time per loop, because there are several hundred of them and the shape of the curve is what is read
// here rather than the last digit of any one point.
BENCHMARK("bench-hash - full length sweep")
{
    cc::random rng(0xABCDEFu);
    for (auto const length : hash_lengths())
    {
        auto const point = length_point(length, rng);
        run_length(point, {.min_time_secs = 0.02, .max_samples = 64, .no_baseline = true}, cc::format(" @{}", length));
    }
}

// Lean PGO benchmark: the representative lengths only, recorded for the PGO speedup report.
//
// nx::bench::run outside a BENCHMARK reports to nobody and hands its result back, so the same measurement machinery
// produces the console tables above and the tracked numbers here.
PGO_BENCHMARK("bench-hash (xxh3 64/128, raw vs wrapper)")
{
    cc::random rng(0xABCDEFu);
    // Enough samples to actually reach the target precision: a tracked number that wobbles is worse than no
    // number, since dev.py pgo reads it as a speedup.
    auto const cfg = nx::bench::run_config{.min_time_secs = 0.1, .max_samples = 512};

    for (auto const length : representative_lengths)
    {
        auto const point = length_point(length, rng);
        auto const label = length >= 1024 ? cc::format("{}KiB", length / 1024) : cc::format("{}B", length);

        auto const* const bps = &nx::bench::unit_bytes_per_second;
        nx::pgo::report(cc::format("hob64@{}", label), bytes_per_second_of(point.measure("hob64", cfg, hash_hob64)),
                        *bps);
        nx::pgo::report(cc::format("hash128@{}", label), bytes_per_second_of(point.measure("hash128", cfg, hash_h128)),
                        *bps);
        nx::pgo::report(cc::format("xxh64@{}", label), bytes_per_second_of(point.measure("xxh64", cfg, hash_x64)), *bps);
        nx::pgo::report(cc::format("xxh128@{}", label), bytes_per_second_of(point.measure("xxh128", cfg, hash_x128)),
                        *bps);
        nx::pgo::report(cc::format("blake3@{}", label), bytes_per_second_of(point.measure("blake3", cfg, hash_b3)), *bps);
        nx::pgo::report(cc::format("b3raw@{}", label), bytes_per_second_of(point.measure("b3raw", cfg, hash_b3raw)),
                        *bps);
    }
}
