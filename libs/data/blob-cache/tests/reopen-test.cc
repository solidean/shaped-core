#include "cache_fixture.hh"

#include <clean-core/streams/file_stream.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

TEST("bcache finds its entries again after a reopen")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("persist", "entry");

    f.settle_only(f.cache().put(key, make_blob_of_size(8192, 4), {.compute_time_secs = 12.5}));

    f.reopen();

    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());
    CHECK(hit.value().data.size() == 8192);
    CHECK(hit.value().data[100] == make_blob_of_size(8192, 4)[100]);

    // The accounting is re-read from the file, not carried over in memory.
    CHECK(f.cache().get_stats().entry_count == 1);
    CHECK(f.cache().get_stats().stored_bytes >= 8192);
    CHECK(f.errors().empty());
}

TEST("bcache survives being dropped with work still buffered")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.access_epoch_secs = 10; });

    for (auto i = 0; i < 4; ++i)
        f.settle_only(f.cache().put(key_of("persist", cc::format("entry-{}", i)), make_blob("payload")));

    f.clock().advance(100);
    for (auto i = 0; i < 4; ++i)
        f.settle_only(f.cache().get(key_of("persist", cc::format("entry-{}", i))));

    // No flush, no close — just a reopen, which is the closest a test gets to the process being killed here.
    f.reopen();

    // Committed entries are intact; only recency could have been lost, and losing that is harmless by design.
    for (auto i = 0; i < 4; ++i)
        CHECK(f.settle(f.cache().get(key_of("persist", cc::format("entry-{}", i)))).has_value());
}

TEST("bcache recreates a file that is not a database and reports nothing to the caller")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("persist", "before"), make_blob("gone after this")));
    f.cache().close();

    // Garbage where a database was.
    // Not a header we can DROP TABLE against, so the only recovery is to unlink — together with the -wal and -shm siblings, since a stale WAL beside a fresh file is its own corruption.
    {
        auto out = cc::file_write_stream_adapter::create(f.path());
        REQUIRE(out.has_value());
        auto stream = out.value().stream();
        auto const junk = cc::string("this is emphatically not a SQLite database, not even a little bit");
        CHECK(stream.write(junk.as_bytes()).has_value());
        CHECK(stream.flush().has_value());
    }

    f.reopen();

    // Behaves as an empty cache, and nothing about that reached the caller as a failure.
    CHECK(!f.settle(f.cache().get(key_of("persist", "before"))).has_value());
    CHECK(f.cache().get_stats().is_backed_by_storage);
    CHECK(f.cache().opened()->has_value());

    CHECK(f.settle(f.cache().put(key_of("persist", "after"), make_blob("fresh start"))).status == put_status::stored);
    CHECK(blob_text(f.settle(f.cache().get(key_of("persist", "after"))).value().data) == "fresh start");
}

TEST("bcache keeps a hit alive after the cache it came from is gone")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The blob is a pin over its own buffer, owned by whoever holds it and by nobody else — which is what lets a
    // caller keep using a value while the cache that produced it is torn down around it.
    auto f = cache_fixture();
    auto const key = key_of("lifetime", "entry");

    f.settle_only(f.cache().put(key, make_blob("outlives the cache")));
    auto const hit = f.settle(f.cache().get(key));
    REQUIRE(hit.has_value());

    f.cache().close();

    CHECK(blob_text(hit.value().data) == "outlives the cache");
}
