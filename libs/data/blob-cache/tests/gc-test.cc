#include "cache_fixture.hh"

#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

TEST("bcache collects down to the target once it is over the limit")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture(
        [](cache_config& c)
        {
            c.limits.max_total_bytes = 40 * 1024;
            c.limits.target_total_bytes = 20 * 1024;
        });

    for (auto i = 0; i < 20; ++i)
        f.settle_only(f.cache().put(key_of("bulk", cc::format("entry-{}", i)), make_blob_of_size(4096, u8(i + 1))));

    f.settle_only(f.cache().collect_garbage());

    // The cumulative counters, not this pass's: crossing the limit already started a pass on the put path, so an explicit collection afterwards legitimately finds nothing left to do.
    auto const stats = f.cache().get_stats();
    CHECK(stats.entries_evicted > 0);
    CHECK(stats.bytes_reclaimed > 0);

    // The target, not the limit: the gap is the hysteresis that keeps the next put from starting another pass.
    CHECK(stats.stored_bytes <= 20 * 1024);
}

TEST("bcache evicts the cheap bulky cold entry before the dear little hot one")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture(
        [](cache_config& c)
        {
            c.limits.max_total_bytes = 12 * 1024;
            c.limits.target_total_bytes = 6 * 1024;
            c.access_epoch_secs = 1; // so a touch below is actually recorded rather than quantized away
        });

    auto const bulky = key_of("score", "bulky-cheap");
    auto const precious = key_of("score", "small-dear");
    auto const filler = key_of("score", "filler");

    // 8 KiB that took a millisecond to make, against 512 bytes that took ten minutes.
    f.settle_only(f.cache().put(bulky, make_blob_of_size(8192, 1), {.compute_time_secs = 0.001}));
    f.settle_only(f.cache().put(precious, make_blob_of_size(512, 2), {.compute_time_secs = 600}));
    f.settle_only(f.cache().put(filler, make_blob_of_size(8192, 3), {.compute_time_secs = 0.001}));

    f.settle_only(f.cache().collect_garbage());

    CHECK(f.settle(f.cache().get(precious)).has_value()); // survives: expensive per byte of disk it occupies
    CHECK(f.cache().get_stats().stored_bytes <= 6 * 1024);
}

TEST("bcache treats an unrecorded compute cost as unknown rather than free")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // Scoring a missing cost at zero would put every undeclared entry first in line for eviction, which is exactly backwards — nobody said it was cheap, only that nobody said.
    auto f = cache_fixture(
        [](cache_config& c)
        {
            c.limits.max_total_bytes = 12 * 1024;
            c.limits.target_total_bytes = 4 * 1024;
            c.default_compute_time_secs = 10;
        });

    auto const unknown_cost = key_of("score", "unknown");
    auto const known_cheap = key_of("score", "known-cheap");

    f.settle_only(f.cache().put(unknown_cost, make_blob_of_size(4096, 1)));
    f.settle_only(f.cache().put(known_cheap, make_blob_of_size(4096, 2), {.compute_time_secs = 0.0001}));
    f.settle_only(f.cache().put(key_of("score", "filler"), make_blob_of_size(4096, 3), {.compute_time_secs = 0.0001}));

    f.settle_only(f.cache().collect_garbage());

    // The default is far above the declared cheap cost, so the undeclared entry outranks it.
    CHECK(f.settle(f.cache().get(unknown_cost)).has_value());
    CHECK(!f.settle(f.cache().get(known_cheap)).has_value());
}

TEST("bcache frees nothing until the last entry naming an object is gone")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The deduplication property, and the reason an eviction phase must never stop because a batch freed zero bytes.
    auto f = cache_fixture();
    auto const shared = make_blob_of_size(4096, 5);

    f.settle_only(f.cache().put(key_of("dedup", "one"), shared));
    f.settle_only(f.cache().put(key_of("dedup", "two"), shared));

    f.settle_only(f.cache().collect_garbage());
    auto const with_both = f.cache().get_stats().stored_bytes;
    CHECK(with_both >= 4096);

    CHECK(f.settle(f.cache().invalidate(key_of("dedup", "one"))));
    auto const after_first = f.settle(f.cache().collect_garbage());

    // Zero bytes freed, because the object still has a live reference.
    CHECK(after_first.objects_reclaimed == 0);
    CHECK(after_first.bytes_reclaimed == 0);
    CHECK(f.cache().get_stats().stored_bytes == with_both);
    CHECK(f.settle(f.cache().get(key_of("dedup", "two"))).has_value());

    CHECK(f.settle(f.cache().invalidate(key_of("dedup", "two"))));
    auto const after_second = f.settle(f.cache().collect_garbage());

    CHECK(after_second.objects_reclaimed == 1);
    CHECK(after_second.bytes_reclaimed >= 4096);
    CHECK(f.cache().get_stats().stored_bytes == 0);
}

TEST("bcache enforces a max entry count as well as a byte ceiling")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture(
        [](cache_config& c)
        {
            c.limits.max_total_bytes = i64(1) << 30; // never the binding constraint here
            c.limits.max_entries = 4;
        });

    for (auto i = 0; i < 12; ++i)
        f.settle_only(f.cache().put(key_of("count", cc::format("entry-{}", i)), make_blob_of_size(64, u8(i + 1))));

    f.settle_only(f.cache().collect_garbage());
    CHECK(f.cache().get_stats().entry_count <= 4);
}

TEST("bcache leaves a cache under its limits untouched")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.limits.max_total_bytes = i64(1) << 30; });

    f.settle_only(f.cache().put(key_of("calm", "a"), make_blob("small")));
    f.settle_only(f.cache().put(key_of("calm", "b"), make_blob("also small")));

    auto const collected = f.settle(f.cache().collect_garbage());
    CHECK(collected.entries_expired == 0);
    CHECK(collected.entries_evicted == 0);
    CHECK(collected.objects_reclaimed == 0);

    CHECK(f.settle(f.cache().get(key_of("calm", "a"))).has_value());
    CHECK(f.settle(f.cache().get(key_of("calm", "b"))).has_value());
}

TEST("bcache set_limits takes effect on the next pass")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture([](cache_config& c) { c.limits.max_total_bytes = i64(1) << 30; });

    for (auto i = 0; i < 8; ++i)
        f.settle_only(f.cache().put(key_of("limits", cc::format("entry-{}", i)), make_blob_of_size(4096, u8(i + 1))));

    f.settle_only(f.cache().collect_garbage());
    CHECK(f.cache().get_stats().entry_count == 8);

    f.cache().set_limits({.max_total_bytes = 8 * 1024, .target_total_bytes = 4 * 1024});
    CHECK(f.cache().get_limits().max_total_bytes == 8 * 1024);

    f.settle_only(f.cache().collect_garbage());
    CHECK(f.cache().get_stats().stored_bytes <= 4 * 1024);
}

TEST("bcache reports a file size larger than the payload it accounts for")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    f.settle_only(f.cache().put(key_of("size", "one"), make_blob_of_size(16384, 1)));
    f.settle_only(f.cache().collect_garbage());

    auto const stats = f.cache().get_stats();
    CHECK(stats.stored_bytes >= 16384);

    // The limits count decoded payload; the file also carries pages, indexes, the freelist and the WAL.
    // Exposing both is what lets a caller reason about the policy and about the disk separately.
    CHECK(stats.file_bytes > 0);
    CHECK(stats.file_bytes >= stats.stored_bytes);
}
