#include "cache_fixture.hh"

#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// Expiry is a LOGICAL property, checked at lookup.
// An entry is a miss the instant it expires, whether or not any collection has run — which is what lets a caller cache short-lived data aggressively without it ever being served stale.

TEST("bcache treats an expired entry as a miss before anything deletes it")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("downloads", "manifest");

    f.settle_only(f.cache().put(key, make_blob("fresh"), {.ttl_secs = 60}));
    CHECK(blob_text(f.settle(f.cache().get(key)).value().data) == "fresh");

    f.clock().advance(61);

    CHECK(!f.settle(f.cache().get(key)).has_value());
    CHECK(f.cache().get_stats().expired_as_miss == 1);

    // Still physically there — the read path never takes a write lock, so nothing was deleted to answer that miss.
    CHECK(f.cache().get_stats().entry_count == 1);
}

TEST("bcache collects an expired entry and the object behind it")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("downloads", "temporary");

    f.settle_only(f.cache().put(key, make_blob_of_size(4096, 3), {.ttl_secs = 30}));
    CHECK(f.cache().get_stats().stored_bytes >= 4096);

    f.clock().advance(31);

    auto const collected = f.settle(f.cache().collect_garbage());
    CHECK(collected.entries_expired == 1);
    CHECK(collected.objects_reclaimed == 1);
    CHECK(collected.bytes_reclaimed >= 4096);

    CHECK(f.cache().get_stats().entry_count == 0);
    CHECK(f.cache().get_stats().stored_bytes == 0);
}

TEST("bcache tells a ttl of zero apart from no ttl at all")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // What the optional buys: absent means never, and 0 means already expired.
    // A sentinel-carrying double could only ever have meant one of the two.
    auto f = cache_fixture();
    auto const never = key_of("ttl", "absent");
    auto const immediate = key_of("ttl", "zero");

    f.settle_only(f.cache().put(never, make_blob("no ttl")));
    f.settle_only(f.cache().put(immediate, make_blob("ttl of zero"), {.ttl_secs = 0}));

    CHECK(f.settle(f.cache().get(never)).has_value());
    CHECK(!f.settle(f.cache().get(immediate)).has_value());
    CHECK(f.cache().get_stats().expired_as_miss == 1);
}

TEST("bcache leaves an entry with no ttl alone forever")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const permanent = key_of("shader", "permanent");
    auto const temporary = key_of("shader", "temporary");

    f.settle_only(f.cache().put(permanent, make_blob("keep me")));
    f.settle_only(f.cache().put(temporary, make_blob("drop me"), {.ttl_secs = 10}));

    f.clock().advance(3600 * 24 * 365);
    f.settle_only(f.cache().collect_garbage());

    CHECK(blob_text(f.settle(f.cache().get(permanent)).value().data) == "keep me");
    CHECK(!f.settle(f.cache().get(temporary)).has_value());
}

TEST("bcache expiry beats eviction scoring")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The point of TTLs in a shared cache: a big short-lived artifact must not be able to crowd out unrelated durable content, however expensive that artifact was to make.
    auto f = cache_fixture([](cache_config& c) { c.limits.max_total_bytes = 1 << 30; });

    auto const expensive_but_expiring = key_of("temp", "artifact");
    auto const cheap_but_permanent = key_of("keep", "small");

    f.settle_only(
        f.cache().put(expensive_but_expiring, make_blob_of_size(8192, 1), {.ttl_secs = 60, .compute_time_secs = 600}));
    f.settle_only(f.cache().put(cheap_but_permanent, make_blob_of_size(64, 2), {.compute_time_secs = 0.001}));

    f.clock().advance(61);
    auto const collected = f.settle(f.cache().collect_garbage());

    // Nothing was over any limit, so the ONLY thing collected is the expired one — the expensive one.
    CHECK(collected.entries_expired == 1);
    CHECK(collected.entries_evicted == 0);
    CHECK(!f.settle(f.cache().get(expensive_but_expiring)).has_value());
    CHECK(f.settle(f.cache().get(cheap_but_permanent)).has_value());
}

TEST("bcache acquire recomputes once its ttl has run out")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("downloads", "http");

    auto calls = 0;
    auto const fetch = [&]
    {
        ++calls;
        return make_blob(calls == 1 ? "first fetch" : "second fetch");
    };

    CHECK(blob_text(f.settle(f.cache().acquire(key, fetch, {.put = {.ttl_secs = 100}}))) == "first fetch");
    f.idle();

    CHECK(blob_text(f.settle(f.cache().acquire(key, fetch, {.put = {.ttl_secs = 100}}))) == "first fetch");
    CHECK(calls == 1);

    f.clock().advance(101);

    // Expired, so the entry is a miss — but it is still THERE, and entries are immutable, so the recomputed value cannot replace it.
    // What the caller gets back is what it just computed, which is the contract.
    CHECK(blob_text(f.settle(f.cache().acquire(key, fetch, {.put = {.ttl_secs = 100}}))) == "second fetch");
    CHECK(calls == 2);
}
