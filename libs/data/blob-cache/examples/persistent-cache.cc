// TODO(nexus): an ASYNC_EXAMPLE, so the body IS an async<unit> and awaits the cache directly.
// Everything here is async underneath, and the example has to say so twice: once by installing a default pool, and once by blocking_get on every call.
// Both are scaffolding around the demonstration rather than part of it.
// ASYNC_TEST already carries the shape (nexus/async-test.hh); the example bucket wants the same.

#include <blob-cache/blob_cache.hh>
#include <blob-cache/keys.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// Stands in for something genuinely expensive — a shader compile, a mesh boolean, a baked texture.
bcache::blob expensive_derivation(cc::string_view input)
{
    auto const text = cc::format("derived({})", input);
    auto data = cc::pinned_data<byte>::create_uninitialized(text.size());
    cc::memcpy(data.data(), text.data(), size_t(text.size()));
    return bcache::blob(data);
}

cc::string blob_text(bcache::blob const& b)
{
    return cc::string(cc::string_view(reinterpret_cast<char const*>(b.data()), b.size()));
}
} // namespace

EXAMPLE("blob-cache/persistent-cache")
{
    if (!bcache::blob_cache::is_storage_available())
    {
        cc::println("no SQLite backend was compiled in — the cache would miss on everything");
        return;
    }

    // A fixed path, deliberately not cleaned up: run this example twice and the second run is a hit.
    // That is the whole point of the library — the cache outlives the process, and other processes share it.
    auto const path = cc::format("{}/bcache-example.db", cc::temp_directory_path());
    cc::println("cache file: {}", path);

    auto cache = bcache::blob_cache::create({.path = path});

    // The cache is an optimization and nothing else, so opening it cannot fail in a way a caller must handle.
    // A storage problem shows up as a miss, never as an error.
    // The cache's actor thread completes async nodes, and a completed node's continuation needs somewhere to run.
    // Installing a default pool is that somewhere — without one, the first completion off a worker thread asserts.
    auto pool = cc::async_thread_pool();
    auto const default_pool = cc::scoped_default_async_pool(pool);

    auto computed = false;

    auto const key = bcache::cache_key{.space = bcache::cache_namespace("examples.derivation"),
                                       .key = bcache::logical_key::create_from_string("teapot"),
                                       .version = bcache::version(1)};

    // acquire is lookup, singleflight and store in one call: the callback runs only on a real miss,
    // and concurrent callers for the same key join one pipeline instead of each computing their own.
    auto const value = pool.blocking_get(cache->acquire(key,
                                                        [&]
                                                        {
                                                            computed = true;
                                                            return expensive_derivation("teapot");
                                                        }));

    cc::println("value: {}", blob_text(value));
    cc::println("this run {}", computed ? "computed it" : "read it from the cache");

    // Asking again in the same process never reaches storage at all.
    auto const again = pool.blocking_get(cache->acquire(key, [&] { return expensive_derivation("teapot"); }));
    cc::println("second acquire returns the same bytes: {}", blob_text(again) == blob_text(value));

    auto const stats = cache->get_stats();
    cc::println("hits {}, misses {}, computes {}, stored bytes {}", stats.hits, stats.misses, stats.computes_started,
                stats.stored_bytes);

    // close() flushes and joins; the destructor would do it too, and calling it twice is fine.
    cache->close();
    cc::println("run this example again to see the hit path");
}
