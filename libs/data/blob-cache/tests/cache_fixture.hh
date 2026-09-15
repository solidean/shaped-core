#pragma once

#include <blob-cache/blob_cache.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>

/// What makes these tests deterministic rather than timed.
///
/// Three things are pinned down, and every test here depends on all three:
///
///   the CLOCKS are injected, so a TTL elapses because the test said so and never because a machine was slow;
///   the ACTOR is unthreaded, so message order is the test's own and a get really does follow the put before it;
///   every test is an ASYNC_TEST with main_thread, awaiting the cache rather than pumping it.
///
/// The third is the one that is easy to get wrong.
/// An unthreaded store runs only when the loop that owns it sweeps, and in a test run that loop is nexus's on the main
/// thread — so a test pumping the store itself from a pool thread races that loop for the store's replies.

namespace bcache::test
{
class fake_clock;
class cache_fixture;
} // namespace bcache::test

/// A clock the test moves by hand.
/// Atomic because the threaded test reads it from several threads at once.
class bcache::test::fake_clock
{
public:
    [[nodiscard]] double now() const { return _now.load(cc::memory_order_relaxed); }
    void advance(double secs) { _now.fetch_add(secs, cc::memory_order_relaxed); }
    void set(double secs) { _now.store(secs, cc::memory_order_relaxed); }

private:
    // 2024-01-01, so an expiry written as now + ttl lands in a plausible range rather than near zero.
    cc::atomic<double> _now = {1704067200.0};
};

/// One cache over a private temp file, plus everything needed to drive it.
class bcache::test::cache_fixture
{
public:
    /// `configure` may adjust the config before the cache opens — limits, epochs, whatever the test is about.
    explicit cache_fixture(cc::function_ref<void(cache_config&)> configure = [](cache_config&) {});
    ~cache_fixture();

    cache_fixture(cache_fixture const&) = delete;
    cache_fixture& operator=(cache_fixture const&) = delete;

    [[nodiscard]] blob_cache& cache() const { return *_cache; }
    [[nodiscard]] fake_clock& clock() { return *_clock; }
    [[nodiscard]] cc::string_view path() const { return _path; }

    /// Every storage error the caches this fixture opened have reported so far, in order.
    /// Empty is the normal case.
    [[nodiscard]] cc::span<cc::string const> errors() const { return *_reported; }

    /// Closes and reopens over the same file, which is how the durability tests get a second process' worth of separation without spawning one.
    void reopen(cc::function_ref<void(cache_config&)> configure = [](cache_config&) {});

    /// Opens a SECOND cache over the same file: two connections, which is the multi-writer property without a second process.
    [[nodiscard]] cc::unique_ptr<blob_cache> open_second();

    /// Settles once the cache has finished opening, whether it opened or degraded — what the old synchronous open waited for.
    [[nodiscard]] cc::shared_async<cc::unit> opened() const { return settle_quietly(_cache->opened()); }

    /// Settles once the store has handled every message sent to it so far, since a flush is answered in mailbox order.
    /// For "nothing further happened" checks, and for the fire-and-forget store an acquire queues before it resolves.
    [[nodiscard]] cc::shared_async<cc::unit> idle() const { return _cache->flush(); }

private:
    // A parameter rather than a capture, so the node lives in the coroutine frame.
    static cc::shared_async<cc::unit> settle_quietly(cc::shared_async<cc::unit> node)
    {
        co_await cc::async_settled(node);
    }

    cc::string _path;
    std::shared_ptr<fake_clock> _clock;
    std::shared_ptr<cc::vector<cc::string>> _reported;
    cc::unique_ptr<blob_cache> _cache;
};

namespace bcache::test
{

/// A blob over `text`, owning its own copy.
[[nodiscard]] blob make_blob(cc::string_view text);

/// A blob of `size` bytes, filled from `seed` so two different seeds never collide and one seed always repeats.
[[nodiscard]] blob make_blob_of_size(isize size, u8 seed);

[[nodiscard]] cc::string blob_text(blob const& b);

[[nodiscard]] cache_key key_of(cc::string_view space, cc::string_view key, i32 version = 1);

/// The value a settled node holds, or a default-constructed T when it settled on its error channel.
template <class T>
[[nodiscard]] T value_or_default(cc::shared_async<T> const& node)
{
    if (auto const* v = node->try_value())
        return *v;
    return T();
}
} // namespace bcache::test
