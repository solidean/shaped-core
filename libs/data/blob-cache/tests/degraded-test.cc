#include "cache_fixture.hh"

#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// The disposability invariant, from the caller's side.
//
// No storage, a path that cannot be opened, a limit that rejects everything — none of it may change what a caller computes, and none of it may reach the caller as an error.
// A broken cache is slow.
// That is all it is.

TEST("bcache create_disabled answers every read as a miss and drops every write")
{
    auto cache = blob_cache::create_disabled();
    auto const key = key_of("disabled", "entry");

    CHECK(cache->opened()->has_value());
    CHECK(!cache->get_stats().is_backed_by_storage);

    auto const put = cache->put(key, make_blob("nowhere to go"));
    REQUIRE(put->is_ready());
    CHECK(put->try_value()->status == put_status::unavailable);

    auto const got = cache->get(key);
    REQUIRE(got->is_ready());
    CHECK(!got->try_value()->has_value());

    // Nothing to pump, and pumping anyway is safe — which is what lets a caller pump unconditionally.
    CHECK(!cache->pump());
}

TEST("bcache acquire returns the computed value with no storage at all")
{
    auto cache = blob_cache::create_disabled();
    auto const key = key_of("disabled", "computed");

    auto calls = 0;
    auto const compute = [&]
    {
        ++calls;
        return make_blob("computed anyway");
    };

    auto const a = cache->acquire(key, compute);
    CHECK(blob_text(cc::async_blocking_get_singlethreaded(a)) == "computed anyway");
    CHECK(calls == 1);

    // Singleflight is pure in-process machinery, so it works with no storage behind it — a second concurrent caller still shares one compute.
    auto const b = cache->acquire(key, compute);
    auto const c = cache->acquire(key, compute);
    CHECK(b.get() == c.get());
    CHECK(cache->get_stats().singleflight_joins == 1);
    CHECK(blob_text(cc::async_blocking_get_singlethreaded(b)) == "computed anyway");
    CHECK(blob_text(cc::async_blocking_get_singlethreaded(c)) == "computed anyway");

    // Every acquire recomputes, because nothing is ever stored — degraded, not wrong.
    CHECK(calls == 2);
}

TEST("bcache opens degraded when its directory does not exist")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // clean-core has no directory creation, so the caller makes the directory.
    // Not finding one is not an error:
    // the cache opens degraded and says so, rather than refusing to hand back a handle.
    auto config = cache_config{.path = "no-such-directory-ba9c1f/cache.db", .unthreaded = true};
    auto reported = cc::vector<cc::string>();
    config.on_storage_error = [&](cc::string_view m) { reported.push_back(cc::string(m)); };

    auto cache = blob_cache::create(cc::move(config));
    while (!cache->opened()->is_ready())
        cache->pump();

    CHECK(cache->opened()->has_error()); // the one place the reason is available, for a log line
    CHECK(!cache->get_stats().is_backed_by_storage);
    CHECK(!reported.empty());

    auto const key = key_of("degraded", "entry");
    auto const put = cache->put(key, make_blob("dropped"));
    while (!put->is_ready())
        cache->pump();
    CHECK(put->try_value()->status == put_status::unavailable);

    auto const got = cache->get(key);
    while (!got->is_ready())
        cache->pump();
    CHECK(!got->try_value()->has_value());
}

TEST("bcache acquire keeps a computed value a failing put could not store")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    // The invariant that matters most: a successful computation never becomes a failure because caching it failed.
    auto f = cache_fixture([](cache_config& c) { c.limits.max_object_bytes = 4; });
    auto const key = key_of("degraded", "too-big");

    auto calls = 0;
    auto const compute = [&]
    {
        ++calls;
        return make_blob("far larger than four bytes");
    };

    CHECK(blob_text(f.settle(f.cache().acquire(key, compute))) == "far larger than four bytes");
    CHECK(calls == 1);

    f.idle();
    CHECK(!f.settle(f.cache().get(key)).has_value()); // nothing was stored, as the limit demanded

    // And it stays that way rather than becoming an error on the next attempt.
    CHECK(blob_text(f.settle(f.cache().acquire(key, compute))) == "far larger than four bytes");
    CHECK(calls == 2);
}

TEST("bcache reports a compute failure and nothing else through acquire")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("degraded", "failing");

    auto const a = f.cache().acquire(key,
                                     []
                                     {
                                         return cc::make_async_from_error<blob>(cc::async_error::make_error(
                                             cc::any_error(cc::string("the computation itself failed"))));
                                     });

    f.drive_until([&] { return a->is_ready(); });
    CHECK(a->has_error());

    // Nothing was stored, so the key is untouched and a later caller starts clean.
    f.idle();
    CHECK(!f.settle(f.cache().get(key)).has_value());
}

TEST("bcache answers after close without hanging or crashing")
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto f = cache_fixture();
    auto const key = key_of("closed", "entry");
    f.settle_only(f.cache().put(key, make_blob("before close")));

    f.cache().close();
    CHECK(f.cache().is_closed());
    f.cache().close(); // idempotent

    auto const got = f.cache().get(key);
    REQUIRE(got->is_ready()); // answered immediately rather than queued at a mailbox nobody will drain
    CHECK(!got->try_value()->has_value());

    auto const put = f.cache().put(key, make_blob("after close"));
    REQUIRE(put->is_ready());
    CHECK(put->try_value()->status == put_status::unavailable);
}
