#include <clean-core/common/hash128.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// View a C string's characters (excluding the terminator) as raw bytes.
cc::span<cc::byte const> bytes_of(char const* s, isize n)
{
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(s), n);
}
} // namespace

TEST("hash128 - deterministic for same input and seed")
{
    auto const data = bytes_of("the quick brown fox", 19);
    CHECK(cc::hash128::create(data, 0) == cc::hash128::create(data, 0));
    CHECK(cc::hash128::create(data, 12345) == cc::hash128::create(data, 12345));
}

TEST("hash128 - seed sensitivity")
{
    auto const data = bytes_of("the quick brown fox", 19);
    CHECK(cc::hash128::create(data, 0) != cc::hash128::create(data, 1));
}

TEST("hash128 - data sensitivity")
{
    CHECK(cc::hash128::create(bytes_of("abc", 3), 0) != cc::hash128::create(bytes_of("abd", 3), 0));
}

TEST("hash128 - empty input is well-defined")
{
    cc::span<cc::byte const> const empty;
    CHECK(cc::hash128::create(empty, 0) == cc::hash128::create(empty, 0));
    // distinct seeds still diverge on empty input
    CHECK(cc::hash128::create(empty, 0) != cc::hash128::create(empty, 42));
}

TEST("hash128 - value semantics")
{
    cc::hash128 const h{.low = 0x1122334455667788ull, .high = 0x99aabbccddeeff00ull};
    cc::hash128 const copy = h;
    CHECK(h == copy);
    CHECK(!(h != copy));

    // default operator<=> orders lexicographically by (low, high)
    cc::hash128 const a{.low = 1, .high = 2};
    cc::hash128 const bigger_low{.low = 2, .high = 0};
    cc::hash128 const bigger_high{.low = 1, .high = 3};
    CHECK(a < bigger_low);
    CHECK(a < bigger_high);
    CHECK(hash(h) == h.low);
}

TEST("hash128 - known answer (XXH3 128-bit, v0.8.3)")
{
    // Reference values produced by XXH3_128bits_withSeed; the empty/seed-0 pair
    // is the canonical XXH3-128 constant. These pin the binding to the algorithm.
    SECTION("empty, seed 0")
    {
        cc::span<cc::byte const> const empty;
        auto const h = cc::hash128::create(empty, 0);
        CHECK(h.low == 0x6001c324468d497full);
        CHECK(h.high == 0x99aa06d3014798d8ull);
    }

    SECTION("empty, seed 42")
    {
        cc::span<cc::byte const> const empty;
        auto const h = cc::hash128::create(empty, 42);
        CHECK(h.low == 0x3c1d09e9fe249164ull);
        CHECK(h.high == 0x16c20acd33f7af2full);
    }

    SECTION("\"abc\", seed 0")
    {
        auto const h = cc::hash128::create(bytes_of("abc", 3), 0);
        CHECK(h.low == 0x78af5f94892f3950ull);
        CHECK(h.high == 0x06b05ab6733a6185ull);
    }

    SECTION("\"hello world\", seed 0")
    {
        auto const h = cc::hash128::create(bytes_of("hello world", 11), 0);
        CHECK(h.low == 0xa99b8775cc15b6c7ull);
        CHECK(h.high == 0xdf8d09e93f874900ull);
    }
}
