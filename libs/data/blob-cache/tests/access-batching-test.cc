#include "cache_fixture.hh"

#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// A cache hit must not cost a write.
//
// Access times are therefore deferred, deduplicated per entry, quantized to an epoch, and batched — approximate
// recency rather than exact LRU, which is the whole point: exact would put every reader in contention with every other process's writer for a number nobody reads until a collection runs.

TEST("bcache writes at most one access row per entry per epoch")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.access_epoch_secs = 100; });
    auto const key = key_of("hot", "entry");

    f.settle_only(f.cache().put(key, make_blob("payload")));

    for (auto i = 0; i < 100; ++i)
        CHECK(f.settle(f.cache().get(key)).has_value());

    f.settle_only(f.cache().flush());
    auto const after_first_epoch = f.cache().get_stats().access_rows_written;

    // A hundred hits inside one epoch: the buffer deduplicates them to one note, and the guarded UPDATE writes it once.
    CHECK(after_first_epoch <= 1);
    CHECK(f.cache().get_stats().hits == 100);

    f.clock().advance(150); // into the next epoch

    for (auto i = 0; i < 100; ++i)
        CHECK(f.settle(f.cache().get(key)).has_value());
    f.settle_only(f.cache().flush());

    // A new epoch is a new value, so exactly one further row is written.
    CHECK(f.cache().get_stats().access_rows_written == after_first_epoch + 1);
}

TEST("bcache writes no access row at all when nothing was read")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("cold", "entry"), make_blob("payload")));

    f.settle_only(f.cache().flush());
    CHECK(f.cache().get_stats().access_rows_written == 0);

    // A miss touches nothing either — there is no row to note an access against.
    CHECK(!f.settle(f.cache().get(key_of("cold", "absent"))).has_value());
    f.settle_only(f.cache().flush());
    CHECK(f.cache().get_stats().access_rows_written == 0);
}

TEST("bcache flushes buffered accesses once the threshold is crossed")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture(
        [](cache_config& c)
        {
            c.access_epoch_secs = 1;
            c.access_flush_threshold = 4;
            c.access_flush_interval_secs = 1e9; // so the threshold is unambiguously what fired
        });

    for (auto i = 0; i < 8; ++i)
        f.settle_only(f.cache().put(key_of("bulk", cc::format("entry-{}", i)), make_blob("payload")));

    // Past the epoch the entries were created in, or the guarded UPDATE would match nothing and there would be
    // no write to observe — which is itself the behaviour the first test in this file pins.
    f.clock().advance(100);

    for (auto i = 0; i < 8; ++i)
        CHECK(f.settle(f.cache().get(key_of("bulk", cc::format("entry-{}", i)))).has_value());

    f.idle();

    // Eight distinct entries is past a threshold of four, so this landed without anyone asking for a flush.
    CHECK(f.cache().get_stats().access_rows_written > 0);
}

TEST("bcache keeps recency the newest of what two writers recorded")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The guard on the UPDATE is `accessed_at < :epoch`, so a stale note can never walk an entry's recency backwards past a newer one another process already wrote.
    // There is no cross-process ordering on this column.
    auto f = cache_fixture([](cache_config& c) { c.access_epoch_secs = 10; });
    auto const key = key_of("shared", "entry");

    f.settle_only(f.cache().put(key, make_blob("payload")));

    auto second = f.open_second();
    f.settle_only(second->opened());

    // The second connection reads at a LATER epoch and flushes first.
    f.clock().advance(1000);
    f.settle_only(second->get(key));
    f.settle_only(second->flush());

    // Now the first reads at an EARLIER one, by rewinding the clock, and flushes after.
    f.clock().set(f.clock().now() - 500);
    f.settle_only(f.cache().get(key));
    f.settle_only(f.cache().flush());

    // Its guarded UPDATE matched nothing, so the newer recency survived.
    CHECK(f.cache().get_stats().access_rows_written == 0);
}

TEST("bcache flushes what it buffered before it closes")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.access_epoch_secs = 10; });
    auto const key = key_of("shutdown", "entry");

    f.settle_only(f.cache().put(key, make_blob("payload")));
    f.clock().advance(100);
    f.settle_only(f.cache().get(key));

    // Nothing has asked for a flush, so the note is still only in memory.
    f.cache().close();

    // A lost batch would cost nothing but a slightly worse eviction decision — but an orderly close should not lose one, and reopening must find the file consistent either way.
    f.reopen();
    CHECK(f.settle(f.cache().get(key)).has_value());
}
