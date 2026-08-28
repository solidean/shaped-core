// zstd and lz4 across their level ranges, on payload shapes shaped-core actually stores.
//
// This is what ../../docs/systems/compression.md cites, and it exists because upstream's own numbers are measured on
// Silesia — a corpus of tarred novels, executables and medical images that nothing here resembles.
// The ratio and the throughput both move a long way once the payload is 300 bytes of JSON rather than 10 MB of prose,
// and the level worth paying for moves with them.
//
// Run it with
//   uv run dev.py benchmark "bench-compress"

#include <clean-core/bytes/compression.hh>
#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/bench/run.hh>
#include <nexus/bench/units.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
using algo = cc::compression_algorithm;

[[nodiscard]] cc::vector<byte> bytes_of(cc::string_view s)
{
    auto out = cc::vector<byte>::create_uninitialized(s.size());
    for (isize i = 0; i < s.size(); ++i)
        out[i] = byte(u8(s[i]));
    return out;
}

/// One vdoc op's assignment blob, roughly: a few ids and a small value, repeated with varying content.
[[nodiscard]] cc::vector<byte> vdoc_op(u64 seed)
{
    auto rng = cc::random(seed);
    auto const text
        = cc::format("{{\"entity\":\"node-{}\",\"component\":\"transform\",\"position\":[{},{},{}]}}",
                     rng.next_u32() % 100000, rng.next_u32() % 1000, rng.next_u32() % 1000, rng.next_u32() % 1000);
    return bytes_of(text);
}

/// A document-sized payload: many such records concatenated, which is what a snapshot blob looks like.
[[nodiscard]] cc::vector<byte> vdoc_document(isize records)
{
    auto out = cc::vector<byte>();
    for (isize i = 0; i < records; ++i)
        out.push_back_range(vdoc_op(u64(i) * 2654435761u));
    return out;
}

/// Stands in for shader bytecode: 4-byte instruction words drawn from a small opcode alphabet, with operands that
/// repeat locally and an occasional immediate that does not.
/// Structured but not text, which is the shape between JSON and noise.
[[nodiscard]] cc::vector<byte> bytecode_like(isize size, u64 seed)
{
    auto rng = cc::random(seed);
    auto out = cc::vector<byte>::create_uninitialized(size);

    u8 regs[4] = {};
    for (isize i = 0; i + 4 <= size; i += 4)
    {
        auto const r = rng.next_u32();
        if (r % 8 == 0) // a new register window every so often, which is what gives the matcher something to find
            for (isize k = 0; k < 4; ++k)
                regs[k] = u8((rng.next_u32() >> 8) & 0x3F);

        out[i + 0] = byte(u8((r >> 8) % 40));                                  // opcode
        out[i + 1] = byte(regs[isize((r >> 16) & 3)]);                         // dst
        out[i + 2] = byte(regs[isize((r >> 18) & 3)]);                         // src
        out[i + 3] = byte(u8(r % 16 == 0 ? (r >> 20) & 0xFF : (r >> 20) % 8)); // operand, occasionally an immediate
    }
    return out;
}

/// One config, measured both ways.
///
/// Two loops rather than one so compression and decompression compare against each other in the table, and the
/// compression RATIO rides along as a recorded quantity: it is the number this benchmark exists for, and it is a
/// property of the payload and the config rather than of the clock.
void measure(cc::string_view label,
             cc::span<byte const> payload,
             cc::compression_config cfg,
             nx::bench::run_config const& run_cfg)
{
    auto compressor = cc::compressor(cfg);
    auto const blob = compressor.compress(payload);

    auto const ratio = blob.size() > 0 ? f64(payload.size()) / f64(blob.size()) : 0.0;
    auto const payload_bytes = f64(payload.size());

    nx::bench::run(cc::format("{} compress", label), run_cfg,
                   [&](nx::bench::iteration& it)
                   {
                       auto const b = compressor.compress(payload);
                       nx::bench::sink(b.size());

                       it.record("bytes", cc::rec::unit_bytes, payload_bytes);
                       it.record("ratio", nx::bench::unit_speedup, ratio);
                   });

    auto decompressor
        = cc::decompressor({.algorithm = cfg.algorithm, .framing = cfg.framing, .dictionary = cfg.dictionary});

    nx::bench::run(cc::format("{} decompress", label), run_cfg,
                   [&](nx::bench::iteration& it)
                   {
                       auto const b = decompressor.decompress(blob);
                       nx::bench::sink(b.has_value() ? b.value().size() : 0);

                       it.record("bytes", cc::rec::unit_bytes, payload_bytes);
                       it.record("ratio", nx::bench::unit_speedup, ratio);
                   });
}

/// Sweeps compare points that measure different amounts of work, so they carry no baseline.
constexpr auto sweep_config = nx::bench::run_config{.min_time_secs = 0.05, .max_samples = 128, .no_baseline = true};

void sweep_levels(cc::string_view shape, cc::span<byte const> payload)
{
    for (auto const level : {-5, -1, 0, 3, 9, 15, 19})
        measure(cc::format("{} zstd {}", shape, level), payload, {.algorithm = algo::zstd, .level = level}, sweep_config);

    for (auto const level : {-8, -1, 0, 6, 12})
        measure(cc::format("{} lz4 {}", shape, level), payload, {.algorithm = algo::lz4, .level = level}, sweep_config);
}
} // namespace

// The two payload shapes shaped-core actually stores: ~256 kB of JSON-ish text, and 1 MB of structured binary.
BENCHMARK("bench-compress - level sweep over our payload shapes")
{
    sweep_levels("json-ish", vdoc_document(4000));
    sweep_levels("bytecode", bytecode_like(1 << 20, 0xB17EC0DE));
}

BENCHMARK("bench-compress - small blobs, with and without a dictionary")
{
    // The case a dictionary exists for, and the one where the level barely matters: a few hundred bytes is too short
    // for the codec to learn anything from before it ends.
    auto const corpus_size = isize(2000);

    auto samples = cc::vector<cc::vector<byte>>();
    for (isize i = 0; i < corpus_size; ++i)
        samples.push_back(vdoc_op(u64(i) * 2654435761u));

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : samples)
        views.push_back(s);

    auto const trained = cc::compression_dictionary::train(algo::zstd, views, 16384);
    REQUIRE(trained.has_value());

    // One representative record, measured both ways.
    // The interesting number is the ratio rather than the throughput: at this size the per-call overhead dominates
    // either way.
    // These four ARE comparable to one another — one payload, four configs — so they keep the baseline column.
    auto const& one = samples[7];
    constexpr auto cfg = nx::bench::run_config{.min_time_secs = 0.05, .max_samples = 128};

    measure("zstd 3, no dict", one, {.algorithm = algo::zstd, .level = 3}, cfg);
    measure("zstd 3, dict", one, {.algorithm = algo::zstd, .level = 3, .dictionary = &trained.value()}, cfg);
    measure("zstd 19, dict", one, {.algorithm = algo::zstd, .level = 19, .dictionary = &trained.value()}, cfg);
    measure("lz4 0, no dict", one, {.algorithm = algo::lz4}, cfg);
}

BENCHMARK("bench-compress - framing overhead on small blobs")
{
    // What `raw` framing is actually worth, which is the whole reason it is an axis rather than an option nobody uses.
    for (auto const records : {isize(1), isize(4), isize(16)})
    {
        auto const payload = vdoc_document(records);

        measure(cc::format("zstd frame {} rec", records), payload, {.algorithm = algo::zstd}, sweep_config);
        measure(cc::format("zstd raw {} rec", records), payload,
                {.algorithm = algo::zstd, .framing = cc::compression_framing::raw}, sweep_config);
        measure(cc::format("lz4 frame {} rec", records), payload, {.algorithm = algo::lz4}, sweep_config);
        measure(cc::format("lz4 raw {} rec", records), payload,
                {.algorithm = algo::lz4, .framing = cc::compression_framing::raw}, sweep_config);
    }
}
