#include "cache_fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

using namespace bcache;
using namespace bcache::test;

ASYNC_TEST("bcache round-trips a blob", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const key = key_of("shader", "vignette");

    auto const stored = co_await f.cache().put(key, make_blob("compiled bytes"));
    CHECK(stored.status == put_status::stored);

    auto const hit = co_await f.cache().get(key);
    REQUIRE(hit.has_value());
    CHECK(blob_text(hit.value().data) == "compiled bytes");
    CHECK(hit.value().hash == stored.hash);
    CHECK(f.errors().empty());
}

ASYNC_TEST("bcache misses on an unknown key, a wrong namespace and a wrong version", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    (void)co_await f.cache().put(key_of("shader", "vignette", 1), make_blob("v1"));

    CHECK(!(co_await f.cache().get(key_of("shader", "bloom", 1))).has_value());
    CHECK(!(co_await f.cache().get(key_of("mesh", "vignette", 1))).has_value());

    // The version is the invalidation mechanism, so a bumped one must not see the old entry.
    CHECK(!(co_await f.cache().get(key_of("shader", "vignette", 2))).has_value());

    CHECK((co_await f.cache().get(key_of("shader", "vignette", 1))).has_value());
}

ASYNC_TEST("bcache stores an empty blob as a hit rather than a miss", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const key = key_of("edge", "empty");

    CHECK((co_await f.cache().put(key, make_blob(""))).status == put_status::stored);

    // Zero bytes is a VALUE.
    // Reporting it as a miss would make "the computation legitimately produced nothing" uncacheable, which is the one case a caller most wants not to repeat.
    auto const hit = co_await f.cache().get(key);
    REQUIRE(hit.has_value());
    CHECK(hit.value().data.size() == 0);
}

ASYNC_TEST("bcache round-trips blobs across the chunk boundary", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    constexpr auto chunk = isize(1) << 20;

    // Just under, exactly on, and just over — the three places a chunking bug lives.
    auto const sizes = cc::vector<isize>{chunk - 1, chunk, chunk + 1, 2 * chunk, 2 * chunk + 7};

    for (auto i = isize(0); i < sizes.size(); ++i)
    {
        auto const key = key_of("blobs", cc::format("size-{}", sizes[i]));
        auto const data = make_blob_of_size(sizes[i], u8(i + 1));

        CHECK((co_await f.cache().put(key, data)).status == put_status::stored);

        auto const hit = co_await f.cache().get(key);
        REQUIRE(hit.has_value());
        REQUIRE(hit.value().data.size() == sizes[i]);
        CHECK(cc::memcmp(hit.value().data.data(), data.data(), size_t(sizes[i])) == 0);
    }
    CHECK(f.errors().empty());
}

ASYNC_TEST("bcache hands back the metadata a put attached", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const key = key_of("shader", "with-meta");

    auto meta = cc::vector<byte>::create_uninitialized(3);
    meta[0] = byte(1);
    meta[1] = byte(2);
    meta[2] = byte(3);

    (void)co_await f.cache().put(key, make_blob("payload"), {.metadata = meta});

    auto const hit = co_await f.cache().get(key);
    REQUIRE(hit.has_value());
    REQUIRE(hit.value().metadata.size() == 3);
    CHECK(hit.value().metadata[1] == byte(2));
}

ASYNC_TEST("bcache refuses an object over max_object_bytes without touching the file", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.limits.max_object_bytes = 16; });

    (void)co_await f.opened();
    auto const key = key_of("big", "too-big");

    auto const put = co_await f.cache().put(key, make_blob_of_size(64, 7));
    CHECK(put.status == put_status::rejected_too_large);

    CHECK(!(co_await f.cache().get(key)).has_value());

    // Rejected, not failed: nothing went wrong, so nothing is reported.
    CHECK(f.errors().empty());
}

ASYNC_TEST("bcache deduplicates identical bytes under different keys", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();

    (void)co_await f.opened();
    auto const data = make_blob("shared payload");

    auto const first = co_await f.cache().put(key_of("a", "one"), data);
    auto const second = co_await f.cache().put(key_of("b", "two"), data);

    CHECK(first.status == put_status::stored);
    CHECK(second.status == put_status::deduplicated); // a new entry, but not one byte written
    CHECK(first.hash == second.hash);

    CHECK(blob_text((co_await f.cache().get(key_of("a", "one"))).value().data) == "shared payload");
    CHECK(blob_text((co_await f.cache().get(key_of("b", "two"))).value().data) == "shared payload");
}

ASYNC_TEST("bcache verifies content hashes when asked to", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.verify_on_read = true; });

    (void)co_await f.opened();
    auto const key = key_of("shader", "verified");

    (void)co_await f.cache().put(key, make_blob("honest bytes"));

    auto const hit = co_await f.cache().get(key);
    REQUIRE(hit.has_value());
    CHECK(blob_text(hit.value().data) == "honest bytes");
    CHECK(f.errors().empty());
}

// The store an acquire queues is awaited by nobody, so the backlog is the only way to know it landed.
ASYNC_TEST("bcache acquire's store has been applied once the backlog settles", main_thread)
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    (void)co_await f.opened();

    auto const key = key_of("shader", "write-behind");
    auto const value = co_await f.cache().acquire(key, [] { return make_blob("stored behind"); });
    CHECK(blob_text(value) == "stored behind");

    co_await cc::async_settled(f.cache().backlog().settled());
    CHECK(f.cache().backlog().outstanding_count() == 0);
    CHECK(f.cache().get_stats().puts_stored == 1);
}
