#include "cache_fixture.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

// The one test that runs the cache the way an application does: a real actor thread and a real work-stealing pool.
//
// Everything else here is unthreaded so message order is the test's own.
// This exists because that determinism is also what would hide a race, and singleflight is the piece a race would break most quietly — two threads both
// missing, both computing, and nobody noticing because both answers are correct.

TEST("bcache singleflights across threads", exclusive())
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    constexpr auto key_count = 8;
    constexpr auto thread_count = 8;
    constexpr auto rounds = 16;

    auto pool = cc::async_thread_pool();
    if (pool.worker_count() == 0)
        SKIP("no threads in this build: the pool runs nothing of its own, so the scenario does not exist");

    auto const install = cc::scoped_default_async_pool(pool);

    auto const path = cc::format("{}/bcache-threaded.db", cc::temp_directory_path());
    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));

    auto computes = cc::vector<cc::atomic<int>>::create_defaulted(key_count);

    {
        auto cache = blob_cache::create({.path = path});
        (void)pool.blocking_get(cache->opened());

        auto const expected = [](int k) { return cc::format("value-for-key-{}", k); };

        for (auto round = 0; round < rounds; ++round)
        {
            auto pending = cc::vector<cc::shared_async<blob>>();
            for (auto t = 0; t < thread_count; ++t)
                for (auto k = 0; k < key_count; ++k)
                    pending.push_back(cache->acquire(key_of("threaded", cc::format("key-{}", k)),
                                                     [&computes, &expected, k]
                                                     {
                                                         computes[k].fetch_add(1, cc::memory_order_relaxed);
                                                         return make_blob(expected(k));
                                                     }));

            for (auto i = isize(0); i < pending.size(); ++i)
            {
                auto const value = pool.blocking_get(pending[i]);
                // The value must be the one belonging to ITS key: a table that mixed two operations up would show here as one key quietly answering with another's bytes.
                CHECK(blob_text(value) == expected(int(i % key_count)));
            }
        }

        for (auto k = 0; k < key_count; ++k)
        {
            // At least one, because the first round must compute; never more than a handful, because after the first store every later round is a hit.
            // A racy table would show a count near thread_count * rounds.
            auto const n = computes[k].load(cc::memory_order_relaxed);
            CHECK(n >= 1);
            CHECK(n <= rounds);
        }
    }

    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));
}

TEST("bcache serves concurrent readers from a real actor thread", exclusive())
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto pool = cc::async_thread_pool();
    if (pool.worker_count() == 0)
        SKIP("no threads in this build: the pool runs nothing of its own, so the scenario does not exist");

    auto const install = cc::scoped_default_async_pool(pool);

    auto const path = cc::format("{}/bcache-threaded-reads.db", cc::temp_directory_path());
    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));

    {
        auto cache = blob_cache::create({.path = path});
        (void)pool.blocking_get(cache->opened());

        auto const key = key_of("threaded", "read-me");
        auto const stored = pool.blocking_get(cache->put(key, make_blob_of_size(64 * 1024, 9)));
        CHECK(stored.status == put_status::stored);

        auto reads = cc::vector<cc::shared_async<cc::optional<cache_hit>>>();
        for (auto i = 0; i < 64; ++i)
            reads.push_back(cache->get(key));

        for (auto const& r : reads)
        {
            auto const hit = pool.blocking_get(r);
            REQUIRE(hit.has_value());
            CHECK(hit.value().data.size() == 64 * 1024);
        }

        // Every hit noted an access, and the actor batched them rather than writing 64 rows.
        (void)pool.blocking_get(cache->flush());
        CHECK(cache->get_stats().hits == 64);
        CHECK(cache->get_stats().access_rows_written <= 1);
    }

    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));
}
