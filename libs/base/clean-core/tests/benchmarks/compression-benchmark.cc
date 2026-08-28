// zstd and lz4 across their level ranges, on payload shapes shaped-core actually stores.
//
// This is what ../../docs/systems/compression.md cites, and it exists because upstream's own numbers are measured on
// Silesia — a corpus of tarred novels, executables and medical images that nothing here resembles.
// The ratio and the throughput both move a long way once the payload is 300 bytes of JSON rather than 10 MB of prose,
// and the level worth paying for moves with them.
//
// Run it with
//   uv run dev.py test "bench-compress" --preset release-clang --timeout 0 --manual

#include "bench_util.hh"

#include <clean-core/bytes/compression.hh>
#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

#include <cstdio>

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

struct row
{
    cc::string label;
    isize original = 0;
    isize compressed = 0;
    double compress_mb_s = 0;
    double decompress_mb_s = 0;
};

[[nodiscard]] row measure(cc::string_view label, cc::span<byte const> payload, cc::compression_config cfg)
{
    auto out = row{.label = cc::string(label), .original = payload.size()};

    auto compressor = cc::compressor(cfg);
    auto const blob = compressor.compress(payload);
    out.compressed = blob.size();

    auto const mb = double(payload.size()) / (1024.0 * 1024.0);

    out.compress_mb_s = bench::measure_units_per_sec(mb,
                                                     [&]
                                                     {
                                                         auto const b = compressor.compress(payload);
                                                         return u64(b.size());
                                                     });

    auto decompressor
        = cc::decompressor({.algorithm = cfg.algorithm, .framing = cfg.framing, .dictionary = cfg.dictionary});
    out.decompress_mb_s = bench::measure_units_per_sec(mb,
                                                       [&]
                                                       {
                                                           auto const b = decompressor.decompress(blob);
                                                           return u64(b.has_value() ? b.value().size() : 0);
                                                       });

    return out;
}

void print_table(cc::string_view title, cc::span<row const> rows)
{
    // cc::string is not null-terminated, so every printf of one goes through the %.*s form.
    std::printf("\n%.*s\n", int(title.size()), title.data());
    std::printf("  %-26s %10s %10s %8s %12s %12s\n", "config", "original", "packed", "ratio", "comp MB/s", "decomp MB/s");
    for (auto const& r : rows)
    {
        auto const ratio = r.compressed > 0 ? double(r.original) / double(r.compressed) : 0.0;
        std::printf("  %-26.*s %10lld %10lld %7.2fx %12.1f %12.1f\n", int(r.label.size()), r.label.data(),
                    (long long)r.original, (long long)r.compressed, ratio, r.compress_mb_s, r.decompress_mb_s);
    }
}

void sweep_levels(cc::string_view title, cc::span<byte const> payload)
{
    auto rows = cc::vector<row>();

    for (auto const level : {-5, -1, 0, 3, 9, 15, 19})
        rows.push_back(measure(cc::format("zstd {}", level), payload, {.algorithm = algo::zstd, .level = level}));

    for (auto const level : {-8, -1, 0, 6, 12})
        rows.push_back(measure(cc::format("lz4 {}", level), payload, {.algorithm = algo::lz4, .level = level}));

    // Deflate is here to be interoperable rather than to win, so what the sweep is for is knowing the size of the gap.
    for (auto const level : {1, 6, 9})
        rows.push_back(measure(cc::format("deflate {}", level), payload, {.algorithm = algo::deflate, .level = level}));

    print_table(title, rows);
}
} // namespace

TEST("bench-compress (level sweep over our payload shapes)", nx::config::manual)
{
    sweep_levels("document-sized vdoc records (~256 kB of JSON-ish text)", vdoc_document(4000));
    sweep_levels("bytecode-like (1 MB of structured binary, not text)", bytecode_like(1 << 20, 0xB17EC0DE));
}

TEST("bench-compress (small blobs, with and without a dictionary)", nx::config::manual)
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
    auto const& one = samples[7];
    auto rows = cc::vector<row>();

    rows.push_back(measure("zstd 3, no dict", one, {.algorithm = algo::zstd, .level = 3}));
    rows.push_back(measure("zstd 3, dict", one, {.algorithm = algo::zstd, .level = 3, .dictionary = &trained.value()}));
    rows.push_back(measure("zstd 19, dict", one, {.algorithm = algo::zstd, .level = 19, .dictionary = &trained.value()}));
    rows.push_back(measure("lz4 0, no dict", one, {.algorithm = algo::lz4}));

    print_table(cc::format("one ~90 byte vdoc record, dictionary trained on {} of them", corpus_size), rows);
}

TEST("bench-compress (framing overhead on small blobs)", nx::config::manual)
{
    // What `raw` framing is actually worth, which is the whole reason it is an axis rather than an option nobody uses.
    auto rows = cc::vector<row>();

    for (auto const records : {isize(1), isize(4), isize(16)})
    {
        auto const payload = vdoc_document(records);

        rows.push_back(measure(cc::format("zstd frame  {} rec", records), payload, {.algorithm = algo::zstd}));
        rows.push_back(measure(cc::format("zstd raw    {} rec", records), payload,
                               {.algorithm = algo::zstd, .framing = cc::compression_framing::raw}));
        rows.push_back(measure(cc::format("lz4 frame   {} rec", records), payload, {.algorithm = algo::lz4}));
        rows.push_back(measure(cc::format("lz4 raw     {} rec", records), payload,
                               {.algorithm = algo::lz4, .framing = cc::compression_framing::raw}));

        // Deflate is the one algorithm with three wrappers rather than two, so its framing row has an extra entry.
        rows.push_back(measure(cc::format("deflate gzip {} rec", records), payload, {.algorithm = algo::deflate}));
        rows.push_back(measure(cc::format("deflate zlib {} rec", records), payload,
                               {.algorithm = algo::deflate, .framing = cc::compression_framing::zlib}));
        rows.push_back(measure(cc::format("deflate raw  {} rec", records), payload,
                               {.algorithm = algo::deflate, .framing = cc::compression_framing::raw}));
    }

    print_table("frame vs raw, small payloads", rows);
}
