#include <clean-core/common/blake3.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// Every upstream test vector hashes the same input: the repeating 251-byte sequence 0, 1, ..., 249, 250, 0, 1, ...
cc::vector<byte> reference_input(isize length)
{
    cc::vector<byte> data;
    data.resize_to_uninitialized(length);
    for (isize i = 0; i < length; ++i)
        data[i] = byte(i % 251);
    return data;
}

cc::string hex_of(cc::hash256 h)
{
    byte digest[32];
    h.to_bytes(digest);

    auto s = cc::string();
    for (auto const b : digest)
    {
        char const digits[] = "0123456789abcdef";
        s.push_back(digits[u8(b) >> 4]);
        s.push_back(digits[u8(b) & 0xF]);
    }
    return s;
}

struct known_answer
{
    isize input_len;
    char const* digest;
};

// The first 32 bytes of each `hash` entry in upstream's test_vectors/test_vectors.json, at the pinned 1.8.6.
// The lengths are chosen to cross every parallelism width the dispatch can pick: a single block, a single
// 1024-byte chunk, and multi-chunk runs wide enough to reach the 4-way (SSE), 8-way (AVX2) and 16-way (AVX512)
// compression paths.
// A three-byte vector alone would prove only that the portable path works.
constexpr known_answer known_answers[] = {
    {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
    {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
    {2, "7b7015bb92cf0b318037702a6cdd81dee41224f734684c2c122cd6359cb1ee63"},
    {3, "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
    {63, "e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
    {64, "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
    {65, "de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
    {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
    {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
    {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
    {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
    {2049, "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
    {4096, "015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969"},
    {4097, "9b4052b38f1c5fc8b1f9ff7ac7b27cd242487b3d890d15c96a1c25b8aa0fb995"},
    {8192, "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
    {8193, "bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b"},
    {16384, "f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4"},
    {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
    {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
};
} // namespace

TEST("blake3 - known answers (BLAKE3 1.8.6 test vectors)")
{
    for (auto const& kat : known_answers)
    {
        auto const data = reference_input(kat.input_len);
        CHECK(hex_of(cc::blake3::create(data)) == kat.digest);
    }
}

TEST("blake3 - streaming equals one-shot for every chunk split")
{
    // 4200 bytes spans four full chunks plus a partial one, so the splits below land inside and across chunks.
    auto const buffer = reference_input(4200);
    cc::span<byte const> const data = buffer;
    auto const expected = cc::blake3::create(data);

    // Splits either side of the 64-byte block and the 1024-byte chunk, which is where a wrong buffer length hides.
    constexpr isize step_sizes[] = {1, 2, 63, 64, 65, 127, 128, 1023, 1024, 1025, 2048, 4200};
    for (isize const step : step_sizes)
    {
        auto hasher = cc::blake3();
        for (isize offset = 0; offset < data.size(); offset += step)
            hasher.update(data.subspan({.offset = offset, .size = cc::min(step, data.size() - offset)}));

        CHECK(hasher.finalize() == expected);
    }
}

TEST("blake3 - streaming with uneven pieces equals one-shot")
{
    auto const buffer = reference_input(3000);
    cc::span<byte const> const data = buffer;

    auto hasher = cc::blake3();
    isize offset = 0;
    for (isize piece = 1; offset < data.size(); piece = piece * 3 + 7)
    {
        auto const n = cc::min(piece, data.size() - offset);
        hasher.update(data.subspan({.offset = offset, .size = n}));
        offset += n;
    }

    CHECK(hasher.finalize() == cc::blake3::create(data));
}

TEST("blake3 - a fresh state hashes the empty input")
{
    cc::span<byte const> const empty;
    CHECK(cc::blake3().finalize() == cc::blake3::create(empty));
    CHECK(hex_of(cc::blake3().finalize()) == known_answers[0].digest);
}

TEST("blake3 - finalize does not consume the state")
{
    auto const data = reference_input(100);

    auto hasher = cc::blake3();
    hasher.update(data);

    auto const first = hasher.finalize();
    CHECK(hasher.finalize() == first);

    // and hashing continues from where finalize left it, over the concatenation of both pieces
    hasher.update(data);

    auto doubled = data;
    doubled.push_back_range(data);
    CHECK(hasher.finalize() == cc::blake3::create(doubled));
    CHECK(hasher.finalize() != first);
}

TEST("blake3 - reset returns to the fresh state")
{
    auto hasher = cc::blake3();
    hasher.update(reference_input(1500));
    hasher.reset();

    CHECK(hasher.finalize() == cc::blake3().finalize());

    hasher.update(reference_input(3));
    CHECK(hex_of(hasher.finalize()) == known_answers[3].digest);
}

TEST("blake3 - distinct inputs give distinct digests")
{
    CHECK(cc::blake3::create(reference_input(64)) != cc::blake3::create(reference_input(65)));
}
