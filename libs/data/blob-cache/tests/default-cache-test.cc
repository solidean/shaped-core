#include "cache_fixture.hh"

#include <blob-cache/default_cache.hh>
#include <blob-cache/impl/cache_paths.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread_pump.hh>
#include <nexus/test.hh>

using namespace bcache;
using namespace bcache::test;

namespace
{
/// A nested path under the OS temp directory that no other test in this binary will pick.
cc::string unique_nested_path()
{
    static auto counter = cc::atomic<i32>(0);
    return cc::format("{}/bcache-dirs-{}/nested/deeper", cc::temp_directory_path(),
                      counter.fetch_add(1, cc::memory_order_relaxed));
}
} // namespace

TEST("bcache names a default cache path under the user's cache directory")
{
    auto const path = default_cache_path();

    CHECK(!path.empty());
    CHECK(cc::string_view(path).ends_with("blob-cache.db"));
    CHECK(cc::string_view(path).contains("shaped-core"));
}

TEST("bcache creates a missing directory tree, and says so again on the second call")
{
    auto const path = unique_nested_path();

    CHECK(impl::create_directories(path));
    CHECK(impl::create_directories(path)); // already there is not a failure

    // An empty path names nothing to create, which is the one input that is an error rather than a no-op.
    CHECK(!impl::create_directories(""));
}

// exclusive() because this one is about the PROCESS-WIDE default, which every other test shares.
TEST("bcache opens a database inside a directory tree it had to create", exclusive())
{
    if (!blob_cache::is_storage_available())
        SKIP("no SQLite backend was compiled in");

    auto const directory = unique_nested_path();
    REQUIRE(impl::create_directories(directory));

    // The whole reason the shim exists: without the directory this open would degrade to a cache that misses forever.
    auto const db_path = cc::format("{}/cache.db", directory);
    auto cache = blob_cache::create({.path = db_path, .unthreaded = true});

    auto opened = cache->opened();
    for (auto i = 0; i < 1000 && !opened->is_ready(); ++i)
        (void)cc::thread_pump_all();

    REQUIRE(opened->is_ready());
    CHECK(cache->get_stats().is_backed_by_storage);

    cache->close();
    CHECK(cc::remove_file(db_path));
}

TEST("bcache lets an installed default nest and unwind")
{
    // Disabled caches throughout: this test is about which pointer comes back, never about storage.
    auto const outer = blob_cache::create_disabled();
    auto const inner = blob_cache::create_disabled();

    // Everything runs under a scope that restores whatever the binary's entry point installed.
    auto const restore = scoped_default_cache(outer.get());
    CHECK(&default_cache() == outer.get());

    {
        auto const nested = scoped_default_cache(inner.get());
        CHECK(&default_cache() == inner.get());
    }

    CHECK(&default_cache() == outer.get());

    // Turning caching off is an installation like any other, so it unwinds the same way.
    {
        auto const nested = scoped_default_cache(nullptr);
        disable_default_cache();
        CHECK(!default_cache().get_stats().is_backed_by_storage);
    }

    CHECK(&default_cache() == outer.get());
}
