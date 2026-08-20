#include <clean-core/bytes/compression.hh>
#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
using algo = cc::compression_algorithm;
using framing = cc::compression_framing;

constexpr algo all_algorithms[] = {algo::zstd, algo::lz4};

[[nodiscard]] cc::vector<byte> repetitive_bytes(isize size)
{
    auto out = cc::vector<byte>::create_uninitialized(size);
    for (isize i = 0; i < size; ++i)
        out[i] = byte(u8('a' + (i / 37) % 7));
    return out;
}

/// Incompressible by construction, which is what exercises the codecs' store-uncompressed fallback and makes
/// compress_bound the only buffer size that can be trusted.
[[nodiscard]] cc::vector<byte> random_bytes(isize size, u64 seed)
{
    auto rng = cc::random(seed);
    auto out = cc::vector<byte>::create_uninitialized(size);
    for (isize i = 0; i < size; ++i)
        out[i] = byte(u8(rng.next_u32() & 0xFF));
    return out;
}

[[nodiscard]] bool same_bytes(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

TEST("compression - framed round trip, every algorithm")
{
    auto const payload = repetitive_bytes(20000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        CHECK(blob.size() < payload.size()); // 20 kB of a 7-symbol cycle had better compress

        auto const back = cc::decompress(blob);
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - raw framing round trip, every algorithm")
{
    auto const payload = repetitive_bytes(9000);

    for (auto const a : all_algorithms)
    {
        auto const cfg = cc::compression_config{.algorithm = a, .framing = framing::raw};
        auto const blob = cc::compress(payload, cfg);

        auto out = cc::vector<byte>::create_uninitialized(payload.size());
        auto const written = cc::decompress_into(blob, out, {.algorithm = a, .framing = framing::raw});
        REQUIRE(written.has_value());
        CHECK(written.value() == payload.size());
        CHECK(same_bytes(out, payload));
    }
}

TEST("compression - raw framing is smaller than framed")
{
    // The whole point of raw: no magic, no declared size, no checksum.
    // On a blob this small that header is most of the file.
    auto const payload = repetitive_bytes(64);

    for (auto const a : all_algorithms)
    {
        auto const framed = cc::compress(payload, {.algorithm = a});
        auto const raw = cc::compress(payload, {.algorithm = a, .framing = framing::raw});
        CHECK(raw.size() < framed.size());
    }
}

TEST("compression - empty input is valid")
{
    cc::span<byte const> const empty;

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(empty, {.algorithm = a});
        auto const back = cc::decompress(blob);
        REQUIRE(back.has_value());
        CHECK(back.value().empty());
    }
}

TEST("compression - a single byte survives")
{
    auto const one = cc::vector<byte>::create_filled(1, byte(0x5A));

    for (auto const a : all_algorithms)
    {
        auto const back = cc::decompress(cc::compress(one, {.algorithm = a}));
        REQUIRE(back.has_value());
        REQUIRE(back.value().size() == 1);
        CHECK(back.value()[0] == byte(0x5A));
    }
}

TEST("compression - incompressible data still round trips")
{
    auto const payload = random_bytes(8192, 0xC0FFEE);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto const back = cc::decompress(blob);
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - compress_bound never under-reports")
{
    // Incompressible input is where a bound that is merely an estimate would be caught out.
    for (auto const a : all_algorithms)
        for (auto const f : {framing::frame, framing::raw})
            for (auto const size : {isize(0), isize(1), isize(97), isize(8192)})
            {
                auto const payload = random_bytes(size, u64(size) * 31 + 7);
                auto const cfg = cc::compression_config{.algorithm = a, .framing = f};

                auto const bound = cc::compress_bound(size, cfg);
                auto out = cc::vector<byte>::create_uninitialized(bound);

                auto const written = cc::compress_into(payload, out, cfg);
                REQUIRE(written.has_value());
                CHECK(written.value() <= bound);
            }
}

TEST("compression - decompress_into with an exact buffer")
{
    auto const payload = repetitive_bytes(5000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto out = cc::vector<byte>::create_uninitialized(payload.size());

        auto const written = cc::decompress_into(blob, out);
        REQUIRE(written.has_value());
        CHECK(written.value() == payload.size());
        CHECK(same_bytes(out, payload));
    }
}

TEST("compression - decompress_into refuses to truncate")
{
    auto const payload = repetitive_bytes(5000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto out = cc::vector<byte>::create_uninitialized(payload.size() / 2);

        CHECK(cc::decompress_into(blob, out).has_error());
    }
}

TEST("compression - detect_algorithm reads the frame magic")
{
    auto const payload = repetitive_bytes(1000);

    for (auto const a : all_algorithms)
    {
        auto const detected = cc::detect_algorithm(cc::compress(payload, {.algorithm = a}));
        REQUIRE(detected.has_value());
        CHECK(detected.value() == a);
    }
}

TEST("compression - detect_algorithm on unframed and on garbage")
{
    // Raw output has had its magic removed on purpose, so it must not be recognized as anything.
    for (auto const a : all_algorithms)
        CHECK(!cc::detect_algorithm(cc::compress(repetitive_bytes(1000), {.algorithm = a, .framing = framing::raw}))
                   .has_value());

    auto const garbage = random_bytes(64, 12345);
    CHECK(!cc::detect_algorithm(garbage).has_value());

    cc::span<byte const> const empty;
    CHECK(!cc::detect_algorithm(empty).has_value());
}

TEST("compression - a framed blob decompresses without being told the algorithm")
{
    auto const payload = repetitive_bytes(3000);

    for (auto const a : all_algorithms)
    {
        auto const back = cc::decompress(cc::compress(payload, {.algorithm = a}));
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - decompressed_size reads the declared size")
{
    auto const payload = repetitive_bytes(7777);

    for (auto const a : all_algorithms)
    {
        auto const size = cc::decompressed_size(cc::compress(payload, {.algorithm = a}));
        REQUIRE(size.has_value());
        CHECK(size.value() == payload.size());
    }
}

TEST("compression - raw framing declares no size")
{
    auto const payload = repetitive_bytes(7777);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a, .framing = framing::raw});
        CHECK(!cc::decompressed_size(blob, {.algorithm = a, .framing = framing::raw}).has_value());
    }
}

TEST("compression - a raw zstd blob can still be grown into, a raw lz4 one cannot")
{
    // The two backends are not symmetric here, and the header says so.
    // Raw zstd is a frame with its magic and flags stripped, so the streaming decoder still knows where it ends.
    // Raw lz4 is a bare block with no length at all.
    auto const payload = repetitive_bytes(6000);

    auto const z = cc::compress(payload, {.algorithm = algo::zstd, .framing = framing::raw});
    auto const back = cc::decompress(z, {.algorithm = algo::zstd, .framing = framing::raw});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));

    auto const l = cc::compress(payload, {.algorithm = algo::lz4, .framing = framing::raw});
    CHECK(cc::decompress(l, {.algorithm = algo::lz4, .framing = framing::raw}).has_error());
}

TEST("compression - max_output_size binds the raw path too")
{
    auto const payload = repetitive_bytes(9000);

    for (auto const a : all_algorithms)
    {
        auto const cfg = cc::compression_config{.algorithm = a, .framing = framing::raw};
        auto const blob = cc::compress(payload, cfg);

        // The buffer is big enough; the limit is what must stop it, on both backends alike.
        auto out = cc::vector<byte>::create_uninitialized(payload.size());
        auto const written
            = cc::decompress_into(blob, out, {.algorithm = a, .framing = framing::raw, .max_output_size = 100});
        auto const bounded = written.has_error() || written.value() <= 100;
        CHECK(bounded);
    }
}

TEST("compression - max_output_size of 0 means zero")
{
    // The growth loop seeds its buffer with a floor so it does not realloc per byte, and that floor must not lift the
    // capacity back over a limit the caller set.
    auto const payload = repetitive_bytes(4000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto const back = cc::decompress(blob, {.algorithm = a, .max_output_size = 0});
        auto const produced_nothing = back.has_error() || back.value().empty();
        CHECK(produced_nothing);
    }
}

TEST("compression - decompressed_size of an empty payload differs by algorithm")
{
    // lz4 spells "no declared size" and "zero bytes" the same way, so it cannot tell them apart; zstd can.
    cc::span<byte const> const empty;

    auto const z = cc::decompressed_size(cc::compress(empty, {.algorithm = algo::zstd}));
    REQUIRE(z.has_value());
    CHECK(z.value() == 0);

    CHECK(!cc::decompressed_size(cc::compress(empty, {.algorithm = algo::lz4})).has_value());
}

TEST("compression - raw framing must be told its algorithm")
{
    auto const blob = cc::compress(repetitive_bytes(500), {.framing = framing::raw});
    auto out = cc::vector<byte>::create_uninitialized(500);

    // Sniffing cannot work here by construction, so leaving the algorithm open is an error rather than a guess.
    CHECK(cc::decompress_into(blob, out, {.framing = framing::raw}).has_error());
}

TEST("compression - max_output_size refuses an oversized payload")
{
    auto const payload = repetitive_bytes(100000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        CHECK(blob.size() * 20 < payload.size()); // a small blob claiming to expand to 100 kB is the shape of a bomb

        CHECK(cc::decompress(blob, {.max_output_size = 1000}).has_error());

        // and the same blob is fine once the limit admits it
        auto const back = cc::decompress(blob, {.max_output_size = 200000});
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - truncated input is an error, not a crash")
{
    auto const payload = repetitive_bytes(20000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto const cut = cc::span<byte const>(blob).first_n(blob.size() / 2);

        CHECK(cc::decompress(cut, {.algorithm = a}).has_error());
    }
}

TEST("compression - corrupt input is an error, not a crash")
{
    auto const payload = repetitive_bytes(20000);

    for (auto const a : all_algorithms)
    {
        auto blob = cc::compress(payload, {.algorithm = a});
        for (isize i = blob.size() / 2; i < blob.size(); i += 3)
            blob[i] = byte(u8(blob[i]) ^ 0xFF);

        // A checksummed frame will reject this, but that is not what the test pins: whether the codec notices or not,
        // what it must never do is read out of bounds or produce more than it was allowed to.
        auto const back = cc::decompress(blob, {.algorithm = a, .max_output_size = 1 << 20});
        auto const stayed_within_bounds = back.has_error() || back.value().size() <= (1 << 20);
        CHECK(stayed_within_bounds);
    }
}

TEST("compression - levels are the algorithm's own scale")
{
    auto const payload = repetitive_bytes(50000);

    // Not "higher is always strictly smaller" — that is not guaranteed for any real input — but every level in each
    // algorithm's documented range must produce something that round trips.
    for (auto const level : {-5, -1, 0, 1, 3, 9, 19})
    {
        auto const back = cc::decompress(cc::compress(payload, {.algorithm = algo::zstd, .level = level}));
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }

    for (auto const level : {-8, -1, 0, 1, 6, 12})
    {
        auto const back = cc::decompress(cc::compress(payload, {.algorithm = algo::lz4, .level = level}));
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - a level beyond the range is clamped, not rejected")
{
    auto const payload = repetitive_bytes(1000);
    auto const back = cc::decompress(cc::compress(payload, {.algorithm = algo::zstd, .level = 1000}));
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));
}

TEST("compression - a reused compressor matches the one-shot form")
{
    auto const payload = repetitive_bytes(4000);

    for (auto const a : all_algorithms)
    {
        auto const cfg = cc::compression_config{.algorithm = a, .level = 5};
        auto c = cc::compressor(cfg);

        // The reuse tier exists for the many-small-blobs case, so the invariant that matters is that holding the
        // context across calls changes nothing about the output.
        for (isize i = 0; i < 4; ++i)
            CHECK(same_bytes(c.compress(payload), cc::compress(payload, cfg)));
    }
}

TEST("compression - a reused decompressor round trips repeatedly")
{
    auto const payload = repetitive_bytes(4000);

    for (auto const a : all_algorithms)
    {
        auto const blob = cc::compress(payload, {.algorithm = a});
        auto d = cc::decompressor({.algorithm = a});

        for (isize i = 0; i < 4; ++i)
        {
            auto const back = d.decompress(blob);
            REQUIRE(back.has_value());
            CHECK(same_bytes(back.value(), payload));
        }
    }
}

TEST("compression - a decompressor with no algorithm still sniffs")
{
    auto d = cc::decompressor({});

    for (auto const a : all_algorithms)
    {
        auto const payload = repetitive_bytes(2000);
        auto const back = d.decompress(cc::compress(payload, {.algorithm = a}));
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - a moved-from compressor destructs cleanly")
{
    auto a = cc::compressor({.algorithm = algo::zstd});
    auto b = cc::move(a);
    CHECK(b.compress(repetitive_bytes(100)).size() > 0);
}

// --- dictionaries -----------------------------------------------------------------------------------------

namespace
{
/// A corpus of short, similar records — the case a dictionary exists for, and the case where each record alone is far
/// too short for the codec to learn anything from.
[[nodiscard]] cc::vector<cc::vector<byte>> sample_corpus(isize count)
{
    auto out = cc::vector<cc::vector<byte>>();
    for (isize i = 0; i < count; ++i)
    {
        auto const text = cc::format("{{\"entity\":\"node-{}\",\"component\":\"transform\",\"visible\":true}}", i);
        out.push_back(cc::vector<byte>::create_copy_of(
            cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size())));
    }
    return out;
}
} // namespace

TEST("compression - a trained zstd dictionary round trips")
{
    auto const corpus = sample_corpus(300);

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : corpus)
        views.push_back(s);

    auto const trained = cc::compression_dictionary::train(algo::zstd, views, 8192);
    REQUIRE(trained.has_value());

    auto const& dict = trained.value();
    CHECK(dict.algorithm() == algo::zstd);
    CHECK(!dict.is_empty());
    CHECK(dict.id() != 0); // a trained zstd dictionary declares an id, which is what a format records

    auto const cfg = cc::compression_config{.algorithm = algo::zstd, .dictionary = &dict};
    auto const back = cc::decompress(cc::compress(corpus[0], cfg), {.algorithm = algo::zstd, .dictionary = &dict});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), corpus[0]));
}

TEST("compression - a dictionary beats no dictionary on short records")
{
    auto const corpus = sample_corpus(300);

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : corpus)
        views.push_back(s);

    auto const trained = cc::compression_dictionary::train(algo::zstd, views, 8192);
    REQUIRE(trained.has_value());

    auto const& dict = trained.value();
    auto const with = cc::compress(corpus[7], {.algorithm = algo::zstd, .dictionary = &dict});
    auto const without = cc::compress(corpus[7], {.algorithm = algo::zstd});

    CHECK(with.size() < without.size());
}

TEST("compression - decompressing without the dictionary fails")
{
    auto const corpus = sample_corpus(300);

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : corpus)
        views.push_back(s);

    auto const trained = cc::compression_dictionary::train(algo::zstd, views, 8192);
    REQUIRE(trained.has_value());

    auto const blob = cc::compress(corpus[3], {.algorithm = algo::zstd, .dictionary = &trained.value()});
    CHECK(cc::decompress(blob, {.algorithm = algo::zstd}).has_error());
}

TEST("compression - a raw-content lz4 dictionary round trips")
{
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::lz4, corpus[0]);

    CHECK(dict.algorithm() == algo::lz4);
    CHECK(dict.id() == 0); // lz4 dictionaries carry no id, so a format must reference the content itself

    auto const cfg = cc::compression_config{.algorithm = algo::lz4, .dictionary = &dict};
    auto const back = cc::decompress(cc::compress(corpus[5], cfg), {.algorithm = algo::lz4, .dictionary = &dict});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), corpus[5]));
}

TEST("compression - an lz4 raw-framed dictionary round trips")
{
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::lz4, corpus[0]);

    auto const cfg = cc::compression_config{.algorithm = algo::lz4, .framing = framing::raw, .dictionary = &dict};
    auto const blob = cc::compress(corpus[5], cfg);

    auto out = cc::vector<byte>::create_uninitialized(corpus[5].size());
    auto const written
        = cc::decompress_into(blob, out, {.algorithm = algo::lz4, .framing = framing::raw, .dictionary = &dict});
    REQUIRE(written.has_value());
    CHECK(same_bytes(out, corpus[5]));
}

TEST("compression - lz4 has no dictionary trainer")
{
    auto const corpus = sample_corpus(20);

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : corpus)
        views.push_back(s);

    CHECK(cc::compression_dictionary::train(algo::lz4, views, 4096).has_error());
}

TEST("compression - a dictionary for the wrong algorithm is rejected")
{
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::lz4, corpus[0]);

    auto out = cc::vector<byte>::create_uninitialized(1024);
    CHECK(cc::compress_into(corpus[1], out, {.algorithm = algo::zstd, .dictionary = &dict}).has_error());
}
