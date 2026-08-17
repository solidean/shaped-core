#include <blob-cache/keys.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace bcache;

TEST("bcache logical keys compare by bytes, never by hash")
{
    auto const a = logical_key::create_from_string("alpha");
    auto const b = logical_key::create_from_string("alpha");
    auto const c = logical_key::create_from_string("beta");

    CHECK(a == b);
    CHECK(!(a == c));
    CHECK(hash(a) == hash(b));

    // A prefix is not the same key, which a length-blind comparison would get wrong.
    CHECK(!(logical_key::create_from_string("ab") == logical_key::create_from_string("abc")));

    // Nor is an embedded NUL a terminator: these bytes are opaque and are stored verbatim.
    auto const with_nul = cc::vector<byte>{byte('a'), byte(0), byte('b')};
    auto const truncated = cc::vector<byte>{byte('a')};
    CHECK(!(logical_key::create_from_bytes(with_nul) == logical_key::create_from_bytes(truncated)));
    CHECK(logical_key::create_from_bytes(with_nul).bytes.size() == 3);
}

TEST("bcache cache keys separate namespace, key and version")
{
    auto const base = cache_key{.space = cache_namespace("shader"),
                                .key = logical_key::create_from_string("vignette"),
                                .version = version(1)};

    auto other_space = base;
    other_space.space = cache_namespace("mesh");
    auto other_version = base;
    other_version.version = version(2);
    auto other_key = base;
    other_key.key = logical_key::create_from_string("bloom");

    CHECK(base == base);
    CHECK(!(base == other_space));
    CHECK(!(base == other_version));
    CHECK(!(base == other_key));

    // Usable as a map key, which is what the singleflight table needs of it.
    auto seen = cc::map<cache_key, int>();
    seen[base] = 1;
    seen[other_version] = 2;
    CHECK(seen.size() == 2);
    CHECK(seen.get(base) == 1);
}

TEST("bcache content hashes round-trip through their durable bytes")
{
    auto const payload = cc::vector<byte>{byte(1), byte(2), byte(3)};
    auto const h = content_hash::create(payload);

    auto bytes = cc::vector<byte>::create_uninitialized(32);
    h.value.to_bytes(bytes);

    // This is the form the objects.hash column holds, so a mismatch here would make every reopen a total miss.
    CHECK(content_hash{cc::hash256::from_bytes(bytes)} == h);

    auto different = payload;
    different[0] = byte(9);
    CHECK(!(content_hash::create(different) == h));
}

TEST("bcache logical keys built from a hash carry all 32 bytes")
{
    auto const payload = cc::vector<byte>{byte(7)};
    auto const key = logical_key::create_from_hash(cc::hash256::create(payload));

    CHECK(key.bytes.size() == 32);
    CHECK(key == logical_key::create_from_hash(cc::hash256::create(payload)));
}
