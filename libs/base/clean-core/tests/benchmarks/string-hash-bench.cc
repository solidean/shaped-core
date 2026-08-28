// Manual throughput benchmark for string hashing.
//
// Compares the production byte-range hash (cc::make_hash_of_bytes, XXH3-64 — what cc::string and cc::string_view hash through today)
// against a couple of hand-rolled "small string" hashers, across a length sweep.
// The question it answers: does XXH3's fixed setup cost make it a poor default for the short keys that dominate hash-table workloads, and where does the crossover sit?
//
// Measurement is GB/s while hashing a large corpus of *distinct* keys back to back — the hash-table insert/lookup scenario, where every key is a cold, never-before-seen string.
// Both a cc::string_view corpus and a cc::string corpus are measured.
// cc::string stores <= 39 bytes inline (SSO), so for short keys it also exercises the small-string layout an actual map would hold.
//
// The PGO_BENCHMARKs record a few representative throughput points via nx::pgo for the PGO speedup report.
// The full length tables come from the sweep BENCHMARKs beside them.
// Run them with `uv run dev.py benchmark "bench-string-hash"`, or sweep the PGO ones with `<binary> --pgo-benchmarks`.

#include <clean-core/common/hash.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/pgo.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// --- hashers under test -------------------------------------------------------------------------------
// Each takes raw (data, size); quality is not asserted here — this measures speed only.

// Production path: XXH3-64 of the bytes (what string/string_view hash through).
u64 hash_xxh3(char const* p, size_t n)
{
    return cc::make_hash_of_bytes(cc::span<byte const>(reinterpret_cast<byte const*>(p), isize(n)));
}

// Classic FNV-1a: one multiply per byte.
// Trivial setup, but byte-at-a-time hurts on longer keys.
u64 hash_fnv1a(char const* p, size_t n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ u64(u8(p[i]))) * 0x100000001b3ull;
    return h;
}

// Word-at-a-time multiply/xor mixer — the kind of cheap hash a hash table might use for short keys.
// Almost no fixed setup, and it processes 8 bytes per step.
// The tail uses overlapping fixed-size reads (wyhash-style) so it never falls back to a variable-length cc::memcpy, which compiles to a slow libc call.
// Not a vetted hash, just a competent speed foil for the small-string regime.
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
            cc::memcpy(&v, p, 8); // constant size -> inlined load
            h = (h ^ v) * k;
            h ^= h >> 29;
            p += 8;
            rem -= 8;
        }
        if (rem > 0)
        {
            u64 v;
            cc::memcpy(&v, p + rem - 8, 8); // last 8 bytes (overlaps, safe since n >= 8)
            h = (h ^ v) * k;
        }
    }
    else if (n >= 4)
    {
        u32 a, b;
        cc::memcpy(&a, p, 4);
        cc::memcpy(&b, p + n - 4, 4); // overlapping first/last 4 bytes
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
// A pile of distinct random keys of a fixed length, laid out back-to-back in one buffer.
// Both the view and the owning representation point at the same content.
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

/// Hashes every key in `keys` once per iteration, recording the bytes covered so the harness derives B/s itself.
/// `keys` is anything iterable whose elements expose .data()/.size().
template <class Keys, class Hasher>
nx::bench::result measure(cc::string_view name, nx::bench::run_config const& cfg, Keys const& keys, Hasher hasher)
{
    auto bytes_per_pass = f64(0);
    for (auto const& k : keys)
        bytes_per_pass += f64(k.size());

    return nx::bench::run(name, cfg,
                          [&](nx::bench::iteration& it)
                          {
                              u64 acc = 0;
                              for (auto const& k : keys)
                                  acc ^= hasher(k.data(), size_t(k.size()));
                              nx::bench::sink(acc);

                              it.items(isize(keys.size())); // keys hashed
                              it.record("bytes", cc::rec::unit_bytes, bytes_per_pass);
                          });
}

/// The byte throughput off a measured loop, which is what the PGO report tracks.
double bytes_per_second_of(nx::bench::result const& r)
{
    auto const* const bytes = r.find_quantity("bytes");
    return bytes != nullptr ? bytes->per_second : 0.0;
}

/// Every hasher over one corpus, as loops of one comparison table.
///
/// `use_strings` picks the owning representation over the view one: cc::string stores <= 39 bytes inline, so for short
/// keys it exercises the small-string layout an actual map would hold.
void run_length(corpus const& c, bool use_strings, nx::bench::run_config const& cfg, cc::string_view suffix)
{
    if (use_strings)
    {
        measure(cc::format("xxh3{}", suffix), cfg, c.strings, hash_xxh3);
        measure(cc::format("fnv1a{}", suffix), cfg, c.strings, hash_fnv1a);
        measure(cc::format("mul{}", suffix), cfg, c.strings, hash_mul);
    }
    else
    {
        measure(cc::format("xxh3{}", suffix), cfg, c.views, hash_xxh3);
        measure(cc::format("fnv1a{}", suffix), cfg, c.views, hash_fnv1a);
        measure(cc::format("mul{}", suffix), cfg, c.views, hash_mul);
    }
}

/// The three hashers at the representative lengths, recorded through nx::pgo.
void record_representative(char const* corpus_kind, bool use_strings, cc::span<isize const> lengths)
{
    cc::random rng(0xC0FFEEu);
    auto const cfg = nx::bench::run_config{.min_time_secs = 0.1, .max_samples = 512};
    auto const* const bps = &nx::bench::unit_bytes_per_second;

    for (auto const length : lengths)
    {
        auto const c = make_corpus(length, rng);
        auto const label = length >= 1024 ? cc::format("{}KiB", length / 1024) : cc::format("{}B", length);
        auto const suffix = cc::format("@{} ({})", label, corpus_kind);

        auto const& keys_views = c.views;
        auto const& keys_strings = c.strings;

        if (use_strings)
        {
            nx::pgo::report(cc::format("xxh3{}", suffix),
                            bytes_per_second_of(measure("xxh3", cfg, keys_strings, hash_xxh3)), *bps);
            nx::pgo::report(cc::format("fnv1a{}", suffix),
                            bytes_per_second_of(measure("fnv1a", cfg, keys_strings, hash_fnv1a)), *bps);
            nx::pgo::report(cc::format("mul{}", suffix),
                            bytes_per_second_of(measure("mul", cfg, keys_strings, hash_mul)), *bps);
        }
        else
        {
            nx::pgo::report(cc::format("xxh3{}", suffix),
                            bytes_per_second_of(measure("xxh3", cfg, keys_views, hash_xxh3)), *bps);
            nx::pgo::report(cc::format("fnv1a{}", suffix),
                            bytes_per_second_of(measure("fnv1a", cfg, keys_views, hash_fnv1a)), *bps);
            nx::pgo::report(cc::format("mul{}", suffix), bytes_per_second_of(measure("mul", cfg, keys_views, hash_mul)),
                            *bps);
        }
    }
}

// The representative lengths the PGO benchmarks sweep: one short key (8 B) and one long (64 KiB).
constexpr isize guide_lengths[] = {8, 64 * 1024};
} // namespace

// Lean PGO benchmarks: just the representative lengths, recorded for the PGO speedup report.
PGO_BENCHMARK("bench-string-hash (string_view)")
{
    record_representative("string_view", false, guide_lengths);
}

PGO_BENCHMARK("bench-string-hash (string)")
{
    record_representative("string", true, guide_lengths);
}

// The complete length tables the docs analyze.
//
// Each length regenerates a multi-MB corpus, so a lower min_time per loop: the shape of the curve is what is read
// here, not the last digit of any one point.
BENCHMARK("bench-string-hash - string_view sweep")
{
    cc::random rng(0xC0FFEEu);
    for (auto const length : make_lengths())
    {
        auto const c = make_corpus(length, rng);
        run_length(c, false, {.min_time_secs = 0.02, .max_samples = 64, .no_baseline = true}, cc::format(" @{}", length));
    }
}

BENCHMARK("bench-string-hash - string sweep")
{
    cc::random rng(0xC0FFEEu);
    for (auto const length : make_lengths())
    {
        auto const c = make_corpus(length, rng);
        run_length(c, true, {.min_time_secs = 0.02, .max_samples = 64, .no_baseline = true}, cc::format(" @{}", length));
    }
}
