#include "cache_fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace bcache;
using namespace bcache::test;

TEST("bcache round-trips a blob")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "vignette");

    auto const stored = f.settle(f.cache().put(key, make_blob("compiled bytes")));
    CHECK(stored.status == put_status::stored);

    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    CHECK(blob_text(hit.value().data) == "compiled bytes");
    CHECK(hit.value().hash == stored.hash);
    CHECK(f.errors().empty());
}

TEST("bcache misses on an unknown key, a wrong namespace and a wrong version")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("shader", "vignette", 1), make_blob("v1")));

    CHECK(!f.settle(f.cache().get(key_of("shader", "bloom", 1))).has_value());
    CHECK(!f.settle(f.cache().get(key_of("mesh", "vignette", 1))).has_value());

    // The version is the invalidation mechanism, so a bumped one must not see the old entry.
    CHECK(!f.settle(f.cache().get(key_of("shader", "vignette", 2))).has_value());

    CHECK(f.settle(f.cache().get(key_of("shader", "vignette", 1))).has_value());
}

TEST("bcache stores an empty blob as a hit rather than a miss")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("edge", "empty");

    CHECK(f.settle(f.cache().put(key, make_blob(""))).status == put_status::stored);

    // Zero bytes is a VALUE.
    // Reporting it as a miss would make "the computation legitimately produced nothing" uncacheable, which is the one case a caller most wants not to repeat.
    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    CHECK(hit.value().data.size() == 0);
}

TEST("bcache round-trips blobs across the chunk boundary")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    constexpr auto chunk = isize(1) << 20;

    // Just under, exactly on, and just over — the three places a chunking bug lives.
    auto const sizes = cc::vector<isize>{chunk - 1, chunk, chunk + 1, 2 * chunk, 2 * chunk + 7};

    for (auto i = isize(0); i < sizes.size(); ++i)
    {
        auto const key = key_of("blobs", cc::format("size-{}", sizes[i]));
        auto const data = make_blob_of_size(sizes[i], u8(i + 1));

        CHECK(f.settle(f.cache().put(key, data)).status == put_status::stored);

        auto const hit = f.settle(f.cache().get(key));
        REQUIRE(hit.has_value());
        REQUIRE(hit.value().data.size() == sizes[i]);
        CHECK(cc::memcmp(hit.value().data.data(), data.data(), size_t(sizes[i])) == 0);
    }
    CHECK(f.errors().empty());
}

TEST("bcache hands back the metadata a put attached")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("shader", "with-meta");

    auto meta = cc::vector<byte>::create_uninitialized(3);
    meta[0] = byte(1);
    meta[1] = byte(2);
    meta[2] = byte(3);

    f.settle_only(f.cache().put(key, make_blob("payload"), {.metadata = meta}));

    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    REQUIRE(hit.value().metadata.size() == 3);
    CHECK(hit.value().metadata[1] == byte(2));
}

TEST("bcache refuses an object over max_object_bytes without touching the file")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.limits.max_object_bytes = 16; });
    auto const key = key_of("big", "too-big");

    auto const put = f.settle(f.cache().put(key, make_blob_of_size(64, 7)));
    CHECK(put.status == put_status::rejected_too_large);

    CHECK(!f.settle(f.cache().get(key)).has_value());

    // Rejected, not failed: nothing went wrong, so nothing is reported.
    CHECK(f.errors().empty());
}

TEST("bcache deduplicates identical bytes under different keys")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const data = make_blob("shared payload");

    auto const first = f.settle(f.cache().put(key_of("a", "one"), data));
    auto const second = f.settle(f.cache().put(key_of("b", "two"), data));

    CHECK(first.status == put_status::stored);
    CHECK(second.status == put_status::deduplicated); // a new entry, but not one byte written
    CHECK(first.hash == second.hash);

    CHECK(blob_text(f.settle(f.cache().get(key_of("a", "one"))).value().data) == "shared payload");
    CHECK(blob_text(f.settle(f.cache().get(key_of("b", "two"))).value().data) == "shared payload");
}

TEST("bcache verifies content hashes when asked to")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.verify_on_read = true; });
    auto const key = key_of("shader", "verified");

    f.settle_only(f.cache().put(key, make_blob("honest bytes")));

    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    CHECK(blob_text(hit.value().data) == "honest bytes");
    CHECK(f.errors().empty());
}
