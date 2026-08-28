#include <clean-core/bytes/compression.hh>
#include <clean-core/bytes/compression_dictionary.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/random.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
using algo = cc::compression_algorithm;
using framing = cc::compression_framing;

constexpr algo all_algorithms[] = {algo::zstd, algo::lz4, algo::deflate};

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

// --- deflate ----------------------------------------------------------------------------------------------
//
// Deflate is the only algorithm with three container formats rather than two, so most of what is worth pinning here is
// about which framing means which wrapper and what each one can carry.

TEST("compression - deflate's framed output really is a gzip file")
{
    // The point of deflate is that something else reads the bytes, so the wrapper is the contract rather than an
    // implementation detail: 1f 8b is gzip's magic and 08 is the only compression method gzip ever uses.
    auto const blob = cc::compress(repetitive_bytes(2000), {.algorithm = algo::deflate});

    REQUIRE(blob.size() > 18);
    CHECK(u8(blob[0]) == 0x1f);
    CHECK(u8(blob[1]) == 0x8b);
    CHECK(u8(blob[2]) == 0x08);
}

TEST("compression - deflate's zlib framing round trips")
{
    auto const payload = repetitive_bytes(9000);
    auto const cfg = cc::compression_config{.algorithm = algo::deflate, .framing = framing::zlib};

    auto const blob = cc::compress(payload, cfg);
    auto const back = cc::decompress(blob, {.algorithm = algo::deflate, .framing = framing::zlib});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));
}

TEST("compression - the zlib wrapper sits between gzip and raw in size")
{
    // gzip carries a 10-byte header plus a CRC-32 and a length; the zlib wrapper carries two bytes plus an Adler-32;
    // raw carries nothing at all.
    auto const payload = repetitive_bytes(64);

    auto const gzip = cc::compress(payload, {.algorithm = algo::deflate});
    auto const zlib = cc::compress(payload, {.algorithm = algo::deflate, .framing = framing::zlib});
    auto const raw = cc::compress(payload, {.algorithm = algo::deflate, .framing = framing::raw});

    CHECK(raw.size() < zlib.size());
    CHECK(zlib.size() < gzip.size());
}

TEST("compression - the zlib wrapper is deliberately not sniffable")
{
    // Its two header bytes are a checksum constraint rather than a magic, so detecting it would claim payload bytes as
    // deflate roughly once in every 31 blobs.
    // A format storing one has to record that it did.
    auto const blob = cc::compress(repetitive_bytes(3000), {.algorithm = algo::deflate, .framing = framing::zlib});

    CHECK(!cc::detect_algorithm(blob).has_value());
    CHECK(cc::decompress(blob, {.framing = framing::zlib}).has_error());
}

TEST("compression - zlib framing belongs to deflate alone")
{
    auto const payload = repetitive_bytes(500);
    auto out = cc::vector<byte>::create_uninitialized(4096);

    for (auto const a : {algo::zstd, algo::lz4})
        CHECK(cc::compress_into(payload, out, {.algorithm = a, .framing = framing::zlib}).has_error());
}

TEST("compression - a framing mismatch is loud rather than guessed at")
{
    // Decoding is strict on purpose: zlib's own 15 + 32 auto-detect would quietly accept a gzip stream where the caller
    // said zlib, and a framing that silently tolerates being wrong is a format bug waiting to happen.
    auto const payload = repetitive_bytes(4000);
    auto const gzip = cc::compress(payload, {.algorithm = algo::deflate});

    CHECK(cc::decompress(gzip, {.algorithm = algo::deflate, .framing = framing::zlib}).has_error());

    auto const zlib = cc::compress(payload, {.algorithm = algo::deflate, .framing = framing::zlib});
    CHECK(cc::decompress(zlib, {.algorithm = algo::deflate}).has_error());
}

TEST("compression - a raw deflate blob can still be grown into")
{
    // Unlike a raw lz4 block, a raw deflate stream is self-terminating: it has no length, but it does say where it
    // ends, so cc::decompress can grow into one without being told the size.
    auto const payload = repetitive_bytes(6000);
    auto const cfg = cc::compression_config{.algorithm = algo::deflate, .framing = framing::raw};

    auto const back = cc::decompress(cc::compress(payload, cfg), {.algorithm = algo::deflate, .framing = framing::raw});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));
}

TEST("compression - deflate levels are zlib's own 1..9")
{
    auto const payload = repetitive_bytes(50000);

    // 0 is the default on this scale rather than zlib's "store", and anything outside 1..9 clamps rather than failing.
    for (auto const level : {-5, 0, 1, 6, 9, 100})
    {
        auto const back = cc::decompress(cc::compress(payload, {.algorithm = algo::deflate, .level = level}));
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), payload));
    }
}

TEST("compression - deflate's compress_bound never under-reports, on every framing")
{
    for (auto const f : {framing::frame, framing::zlib, framing::raw})
        for (auto const size : {isize(0), isize(1), isize(97), isize(8192)})
        {
            auto const payload = random_bytes(size, u64(size) * 17 + 3);
            auto const cfg = cc::compression_config{.algorithm = algo::deflate, .framing = f};

            auto const bound = cc::compress_bound(size, cfg);
            auto out = cc::vector<byte>::create_uninitialized(bound);

            auto const written = cc::compress_into(payload, out, cfg);
            REQUIRE(written.has_value());
            CHECK(written.value() <= bound);
        }
}

TEST("compression - a gzip frame declares its size, the other two do not")
{
    auto const payload = repetitive_bytes(7777);

    auto const declared = cc::decompressed_size(cc::compress(payload, {.algorithm = algo::deflate}));
    REQUIRE(declared.has_value());
    CHECK(declared.value() == payload.size());

    // Unlike lz4, gzip writes its length even for empty content, so 0 here means zero rather than "unknown".
    cc::span<byte const> const empty;
    auto const none = cc::decompressed_size(cc::compress(empty, {.algorithm = algo::deflate}));
    REQUIRE(none.has_value());
    CHECK(none.value() == 0);

    for (auto const f : {framing::zlib, framing::raw})
    {
        auto const blob = cc::compress(payload, {.algorithm = algo::deflate, .framing = f});
        CHECK(!cc::decompressed_size(blob, {.algorithm = algo::deflate, .framing = f}).has_value());
    }
}

TEST("compression - gzip framing cannot carry a dictionary")
{
    // deflateSetDictionary refuses a gzip-wrapped stream, and the gzip header has nowhere to record which dictionary
    // applies — so this is an error rather than a dictionary that is silently ignored.
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::deflate, corpus[0]);

    auto out = cc::vector<byte>::create_uninitialized(4096);
    CHECK(cc::compress_into(corpus[1], out, {.algorithm = algo::deflate, .dictionary = &dict}).has_error());
}

TEST("compression - a deflate dictionary round trips under zlib and raw framing")
{
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::deflate, corpus[0]);

    CHECK(dict.algorithm() == algo::deflate);
    CHECK(dict.id() != 0); // the Adler-32 a zlib header stores as DICTID, which is what a format records

    for (auto const f : {framing::zlib, framing::raw})
    {
        auto const cfg = cc::compression_config{.algorithm = algo::deflate, .framing = f, .dictionary = &dict};
        auto const blob = cc::compress(corpus[5], cfg);

        auto const back = cc::decompress(blob, {.algorithm = algo::deflate, .framing = f, .dictionary = &dict});
        REQUIRE(back.has_value());
        CHECK(same_bytes(back.value(), corpus[5]));
    }
}

TEST("compression - a zlib-framed stream needing a dictionary says so")
{
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::deflate, corpus[0]);

    auto const cfg = cc::compression_config{.algorithm = algo::deflate, .framing = framing::zlib, .dictionary = &dict};
    auto const blob = cc::compress(corpus[3], cfg);

    // The zlib header records the DICTID, so inflate can tell the caller a dictionary is missing rather than producing garbage.
    CHECK(cc::decompress(blob, {.algorithm = algo::deflate, .framing = framing::zlib}).has_error());
}

TEST("compression - deflate has no dictionary trainer")
{
    auto const corpus = sample_corpus(20);

    auto views = cc::vector<cc::span<byte const>>();
    for (auto const& s : corpus)
        views.push_back(s);

    CHECK(cc::compression_dictionary::train(algo::deflate, views, 4096).has_error());
}

// --- deflate interoperability -----------------------------------------------------------------------------
//
// Every test above round-trips through our own encoder, which only ever shows that the codec is self-consistent.
// Deflate exists so that something else reads and writes the bytes, so these decode payloads this codebase did not
// produce, and check the trailer fields a decoder that ignored them would still pass without.

namespace
{
/// A gzip member written by GNU gzip 1.14 (`gzip -9 -n`), byte for byte.
/// -n keeps the name and timestamp out, which is the only reason this is stable enough to paste here.
constexpr byte k_gzip_from_gzip[]
    = {byte(0x1f), byte(0x8b), byte(0x08), byte(0x00), byte(0x00), byte(0x00), byte(0x00), byte(0x00), byte(0x02),
       byte(0x03), byte(0xcb), byte(0x48), byte(0xcd), byte(0xc9), byte(0xc9), byte(0x57), byte(0x48), byte(0x2b),
       byte(0xca), byte(0xcf), byte(0x55), byte(0x48), byte(0x54), byte(0x28), byte(0x4a), byte(0x4d), byte(0xcc),
       byte(0x51), byte(0x48), byte(0xaf), byte(0xca), byte(0x2c), byte(0x50), byte(0x48), byte(0xcb), byte(0xcc),
       byte(0x49), byte(0xd5), byte(0x51), byte(0x28), byte(0x28), byte(0xca), byte(0x4f), byte(0x29), byte(0x4d),
       byte(0x4e), byte(0x4d), byte(0x51), byte(0x48), byte(0xaa), byte(0x84), byte(0x08), byte(0x1b), byte(0xea),
       byte(0x19), byte(0x9a), byte(0x28), byte(0x64), byte(0x96), byte(0x14), byte(0xa7), byte(0xe6), byte(0xa4),
       byte(0xe9), byte(0x71), byte(0x01), byte(0x00), byte(0x17), byte(0xf0), byte(0x3d), byte(0x95), byte(0x3b),
       byte(0x00), byte(0x00), byte(0x00)};

constexpr char k_gzip_plaintext[] = "hello from a real gzip file, produced by gzip 1.14 itself.\n";

/// A PNG IDAT stream: the RFC 1950 wrapper is exactly what `zlib` framing means, and PNG is where most of it is met.
/// One 4x1 RGB scanline — the leading 0 is PNG's per-line filter byte.
constexpr byte k_png_idat[]
    = {byte(0x78), byte(0xda), byte(0x63), byte(0xf8), byte(0xcf), byte(0xc0), byte(0xc0), byte(0x00), byte(0xc6),
       byte(0xff), byte(0xff), byte(0xff), byte(0x07), byte(0x00), byte(0x1d), byte(0xef), byte(0x05), byte(0xfb)};

constexpr u8 k_png_scanline[] = {0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};

/// CRC-32 (the reflected IEEE polynomial gzip uses), computed bitwise on purpose.
/// Checking our trailer against zlib's own crc32() would only prove the two calls agree.
[[nodiscard]] u32 crc32_bitwise(cc::span<byte const> data)
{
    auto crc = u32(0xFFFFFFFF);
    for (auto const b : data)
    {
        crc ^= u32(u8(b));
        for (auto bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return crc ^ 0xFFFFFFFF;
}

[[nodiscard]] u32 le32_at(cc::span<byte const> data, isize at)
{
    return u32(u8(data[at])) | (u32(u8(data[at + 1])) << 8) | (u32(u8(data[at + 2])) << 16)
         | (u32(u8(data[at + 3])) << 24);
}
} // namespace

TEST("compression - a gzip file written by gzip itself decompresses")
{
    auto const back = cc::decompress(k_gzip_from_gzip);
    REQUIRE(back.has_value());

    auto const text = cc::string_view(k_gzip_plaintext);
    REQUIRE(back.value().size() == text.size());
    for (isize i = 0; i < text.size(); ++i)
        CHECK(u8(back.value()[i]) == u8(text[i]));

    // The magic is enough to sniff it, so nothing had to be told what it was.
    CHECK(cc::detect_algorithm(k_gzip_from_gzip) == algo::deflate);
    CHECK(cc::decompressed_size(k_gzip_from_gzip).value() == text.size());
}

TEST("compression - a zlib stream lifted from a PNG IDAT decompresses")
{
    auto const back = cc::decompress(k_png_idat, {.algorithm = algo::deflate, .framing = framing::zlib});
    REQUIRE(back.has_value());

    REQUIRE(back.value().size() == isize(sizeof(k_png_scanline)));
    for (isize i = 0; i < isize(sizeof(k_png_scanline)); ++i)
        CHECK(u8(back.value()[i]) == k_png_scanline[i]);
}

TEST("compression - the gzip trailer carries the payload's CRC-32 and length")
{
    // A decoder that wrote neither field would pass every round-trip test in this file, and every other gzip reader
    // would then reject what it produced.
    auto const payload = repetitive_bytes(5000);
    auto const blob = cc::compress(payload, {.algorithm = algo::deflate});

    REQUIRE(blob.size() > 8);
    CHECK(le32_at(blob, blob.size() - 8) == crc32_bitwise(payload));
    CHECK(le32_at(blob, blob.size() - 4) == u32(payload.size()));
}

TEST("compression - a concatenated gzip file decodes every member")
{
    // `cat a.gz b.gz`, pigz and bgzip all produce this, and it is a legal .gz.
    // zlib's inflate stops at the end of one member, so decoding only the first is the silent failure this pins.
    auto const first = repetitive_bytes(700);
    auto const second = random_bytes(300, 99);

    auto blob = cc::compress(first, {.algorithm = algo::deflate});
    blob.push_back_range(cc::compress(second, {.algorithm = algo::deflate}));

    auto expected = cc::vector<byte>();
    expected.push_back_range(first);
    expected.push_back_range(second);

    auto const back = cc::decompress(blob);
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), expected));

    // ISIZE now describes the LAST member only, which is the second reason it is a hint rather than a size.
    CHECK(cc::decompressed_size(blob).value() == second.size());
}

TEST("compression - trailing garbage after a gzip member is not a member")
{
    // What follows the trailer is not gzip, so the member ends the decode rather than failing it — the same rule
    // every gzip reader applies.
    auto const payload = repetitive_bytes(400);

    auto blob = cc::compress(payload, {.algorithm = algo::deflate});
    for (auto i = 0; i < 32; ++i)
        blob.push_back(byte(u8('x')));

    auto const back = cc::decompress(blob, {.algorithm = algo::deflate});
    REQUIRE(back.has_value());
    CHECK(same_bytes(back.value(), payload));

    // Those 32 bytes put 0x78787878 where ISIZE is read, so the seed allocation would be 2 GB if the declared size
    // were believed — an out-of-memory abort on a 32-bit target, from four bytes anybody can append.
    // DEFLATE cannot expand by more than 1032x, so a hint above that is a lie rather than a large payload.
    CHECK(back.value().capacity() <= blob.size() * 1032 + 4096);
}

TEST("compression - a dictionary under gzip framing is refused with the reason")
{
    // compress_into, the stream adapter and cc::compressor all reach the same rule, and all three have to name it
    // rather than report a context that could not be created.
    auto const corpus = sample_corpus(20);
    auto const dict = cc::compression_dictionary::from_bytes(algo::deflate, corpus[0]);

    auto out = cc::vector<byte>::create_uninitialized(4096);
    auto const written = cc::compress_into(corpus[1], out, {.algorithm = algo::deflate, .dictionary = &dict});
    REQUIRE(written.has_error());
    CHECK(cc::string_view(written.error().to_string()).contains("dictionary"));
}
