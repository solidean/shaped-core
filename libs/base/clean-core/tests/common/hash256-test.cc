#include <clean-core/common/hash256.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// View a C string's characters (excluding the terminator) as raw bytes.
cc::span<byte const> bytes_of(char const* s, isize n)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s), n);
}
} // namespace

TEST("hash256 - deterministic for the same input")
{
    auto const data = bytes_of("the quick brown fox", 19);
    CHECK(cc::hash256::create(data) == cc::hash256::create(data));
}

TEST("hash256 - data sensitivity")
{
    CHECK(cc::hash256::create(bytes_of("abc", 3)) != cc::hash256::create(bytes_of("abd", 3)));
}

TEST("hash256 - empty input is well-defined")
{
    cc::span<byte const> const empty;
    CHECK(cc::hash256::create(empty) == cc::hash256::create(empty));
    CHECK(cc::hash256::create(empty) != cc::hash256::create(bytes_of("a", 1)));
}

TEST("hash256 - value semantics")
{
    cc::hash256 const h = {.l0 = 0x1122334455667788ull, .l1 = 2, .l2 = 3, .l3 = 4};
    cc::hash256 const copy = h;
    CHECK(h == copy);
    CHECK(hash(h) == h.l0);

    // the default operator<=> orders lexicographically by (l0, l1, l2, l3)
    cc::hash256 const a = {.l0 = 1, .l1 = 2, .l2 = 0, .l3 = 0};
    cc::hash256 const bigger_l0 = {.l0 = 2, .l1 = 0, .l2 = 0, .l3 = 0};
    cc::hash256 const bigger_l1 = {.l0 = 1, .l1 = 3, .l2 = 0, .l3 = 0};
    cc::hash256 const bigger_l2 = {.l0 = 1, .l1 = 2, .l2 = 1, .l3 = 0};
    cc::hash256 const bigger_l3 = {.l0 = 1, .l1 = 2, .l2 = 0, .l3 = 1};
    CHECK(a < bigger_l0);
    CHECK(a < bigger_l1);
    CHECK(a < bigger_l2);
    CHECK(a < bigger_l3);
}

TEST("hash256 - bytes round-trip and are little-endian per limb")
{
    cc::hash256 const h = {.l0 = 0x0807060504030201ull, .l1 = 0x100f0e0d0c0b0a09ull, .l2 = 3, .l3 = 4};

    byte raw[32];
    h.to_bytes(raw);

    // l0 first, least significant byte first within each limb
    for (auto i = 0; i < 16; ++i)
        CHECK(u8(raw[i]) == u8(i + 1));

    CHECK(cc::hash256::from_bytes(raw) == h);
}

TEST("hash256 - bytes round-trip a real digest")
{
    auto const h = cc::hash256::create(bytes_of("hello world", 11));

    byte raw[32];
    h.to_bytes(raw);
    CHECK(cc::hash256::from_bytes(raw) == h);
}

TEST("hash256 - works as a map key")
{
    auto keys = cc::map<cc::hash256, int>();
    for (auto i = 0; i < 64; ++i)
    {
        auto const bytes = cc::span<int const>(&i, 1).as_bytes();
        keys[cc::hash256::create(bytes)] = i;
    }

    CHECK(keys.size() == 64);
    for (auto i = 0; i < 64; ++i)
    {
        auto const bytes = cc::span<int const>(&i, 1).as_bytes();
        CHECK(keys.get(cc::hash256::create(bytes)) == i);
    }
}
