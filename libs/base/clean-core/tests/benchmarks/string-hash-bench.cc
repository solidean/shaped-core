// Manual throughput benchmark for string hashing.
//
// Compares the production byte-range hash (cc::make_hash_of_bytes, XXH3-64 — what cc::string and
// cc::string_view hash through today) against a couple of hand-rolled "small string" hashers, across a
// length sweep. The question it answers: does XXH3's fixed setup cost make it a poor default for the short
// keys that dominate hash-table workloads, and where does the crossover sit?
//
// Measurement is GB/s while hashing a large corpus of *distinct* keys back to back — the hash-table insert/
// lookup scenario, where every key is a cold, never-before-seen string. Both a cc::string_view corpus and a
// cc::string corpus are measured: cc::string stores <= 39 bytes inline (SSO), so for short keys it also
// exercises the small-string layout an actual map would hold.
//
// These are manual tests (nx::config::manual): they only print, they never CHECK, and timing is noisy. Run
// them explicitly, e.g. `uv run dev.py test --preset release-clang "bench-string-hash"` (an exact name), or
// put the runner in manual mode and sweep: `<binary> --manual bench`.

#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace cc::primitive_defines;

namespace
{
// --- hashers under test -------------------------------------------------------------------------------
// Each takes raw (data, size); quality is not asserted here — this measures speed only.

// Production path: XXH3-64 of the bytes (what string/string_view hash through).
u64 hash_xxh3(char const* p, size_t n)
{
    return cc::make_hash_of_bytes(cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(p), isize(n)));
}

// Classic FNV-1a: one multiply per byte. Trivial setup, but byte-at-a-time hurts on longer keys.
u64 hash_fnv1a(char const* p, size_t n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ u64(u8(p[i]))) * 0x100000001b3ull;
    return h;
}

// Word-at-a-time multiply/xor mixer — the kind of cheap hash a hash table might use for short keys: almost no
// fixed setup, processes 8 bytes per step. The tail uses overlapping fixed-size reads (wyhash-style) so it
// never falls back to a variable-length std::memcpy, which compiles to a slow libc call. Not a vetted hash,
// just a competent speed foil for the small-string regime.
u64 hash_mul(char const* p, size_t n)
{
    constexpr u64 k = 0xff51afd7ed558ccdull;
    u64 h = 0x9e3779b97f4a7c15ull ^ (u64(n) * k);

    if (n >= 8)
    {
        size_t rem = n;
        while (rem >= 8)
        {
            u64 v;
            std::memcpy(&v, p, 8); // constant size -> inlined load
            h = (h ^ v) * k;
            h ^= h >> 29;
            p += 8;
            rem -= 8;
        }
        if (rem > 0)
        {
            u64 v;
            std::memcpy(&v, p + rem - 8, 8); // last 8 bytes (overlaps, safe since n >= 8)
            h = (h ^ v) * k;
        }
    }
    else if (n >= 4)
    {
        u32 a, b;
        std::memcpy(&a, p, 4);
        std::memcpy(&b, p + n - 4, 4); // overlapping first/last 4 bytes
        h = (h ^ ((u64(a) << 32) | b)) * k;
    }
    else if (n > 0)
    {
        u64 const v = (u64(u8(p[0])) << 16) | (u64(u8(p[n >> 1])) << 8) | u64(u8(p[n - 1]));
        h = (h ^ v) * k;
    }

    h ^= h >> 32;
    h *= k;
    h ^= h >> 32;
    return h;
}

// --- length sweep -------------------------------------------------------------------------------------
// 1..32 (every length), then +8 up to 64, then *1.5 until ~100k.
cc::vector<isize> make_lengths()
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

// --- corpus -----------------------------------------------------------------------------------------
// A pile of distinct random keys of a fixed length, laid out back-to-back in one buffer. Both view and owning
// representations point at the same content.
struct corpus
{
    cc::vector<char> buffer;
    cc::vector<cc::string_view> views;
    cc::vector<cc::string> strings;
    isize length = 0;
};

corpus make_corpus(isize length, cc::random& rng)
{
    // Aim for an ~8 MB working set per length so we stress cache the way a real map would, but cap the key
    // count so tiny lengths don't allocate millions of entries.
    constexpr isize target_bytes = 8 * 1024 * 1024;
    isize count = target_bytes / length;
    count = cc::clamp(count, isize(64), isize(200000));

    corpus c;
    c.length = length;
    c.buffer.resize_to_uninitialized(count * length);
    for (isize i = 0; i < c.buffer.size(); ++i)
        c.buffer[i] = char(rng.uniform(32, 126)); // printable ASCII, no embedded nulls

    c.views.reserve(count);
    c.strings.reserve(count);
    for (isize i = 0; i < count; ++i)
    {
        char const* const p = c.buffer.data() + i * length;
        c.views.push_back(cc::string_view(p, length));
        c.strings.push_back(cc::string(p, length));
    }
    return c;
}

// --- timing -----------------------------------------------------------------------------------------
u64 volatile g_sink = 0;

// Hashes every key in `keys`, repeating the full pass until at least ~50 ms elapsed, and returns GB/s over the
// bytes actually processed. `keys` is anything iterable whose elements expose .data()/.size().
template <class Keys, class Hasher>
double measure_gbps(Keys const& keys, Hasher hasher)
{
    using clock = std::chrono::steady_clock;

    size_t bytes_per_pass = 0;
    for (auto const& k : keys)
        bytes_per_pass += size_t(k.size());

    u64 acc = 0;
    for (auto const& k : keys) // warm caches / branch predictors
        acc ^= hasher(k.data(), size_t(k.size()));

    long long reps = 1;
    double seconds = 0;
    for (;;)
    {
        auto const t0 = clock::now();
        for (long long r = 0; r < reps; ++r)
            for (auto const& k : keys)
                acc ^= hasher(k.data(), size_t(k.size()));
        seconds = std::chrono::duration<double>(clock::now() - t0).count();

        if (seconds >= 0.05 || reps >= (1ll << 22))
            break;
        reps *= 2;
    }
    g_sink = acc; // keep the work observable

    double const total_bytes = double(bytes_per_pass) * double(reps);
    return total_bytes / seconds / 1e9;
}

void run_sweep(char const* corpus_kind, bool use_strings)
{
    auto const lengths = make_lengths();
    cc::random rng(0xC0FFEEu);

    std::printf("\n=== string hash throughput (GB/s) — %s corpus ===\n", corpus_kind);
    std::printf("%8s %12s %12s %12s\n", "length", "xxh3", "fnv1a", "mul");
    std::printf("%8s %12s %12s %12s\n", "------", "------", "------", "------");

    for (isize const length : lengths)
    {
        auto const c = make_corpus(length, rng);

        double gbps_xxh3, gbps_fnv1a, gbps_mul;
        if (use_strings)
        {
            gbps_xxh3 = measure_gbps(c.strings, hash_xxh3);
            gbps_fnv1a = measure_gbps(c.strings, hash_fnv1a);
            gbps_mul = measure_gbps(c.strings, hash_mul);
        }
        else
        {
            gbps_xxh3 = measure_gbps(c.views, hash_xxh3);
            gbps_fnv1a = measure_gbps(c.views, hash_fnv1a);
            gbps_mul = measure_gbps(c.views, hash_mul);
        }

        std::printf("%8lld %12.2f %12.2f %12.2f\n", (long long)length, gbps_xxh3, gbps_fnv1a, gbps_mul);
    }
    std::fflush(stdout);
}
} // namespace

TEST("bench-string-hash (string_view)", nx::config::manual)
{
    run_sweep("string_view", false);
}

TEST("bench-string-hash (string)", nx::config::manual)
{
    run_sweep("string", true);
}
