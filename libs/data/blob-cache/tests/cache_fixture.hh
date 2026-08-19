#pragma once

#include <blob-cache/blob_cache.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/atomic.hh>

/// What makes these tests deterministic rather than timed.
///
/// Three things are pinned down, and every test here depends on all three:
///
///   the CLOCKS are injected, so a TTL elapses because the test said so and never because a machine was slow;
///   the ACTOR is unthreaded, so message order is the test's own and a get really does follow the put before it;
///   the async graph is driven by a scheduler bound to THIS thread, so waking a parked frame needs no pool.
///
/// The third is the one that is easy to get wrong.
/// Completing a node schedules whatever was parked on it, and scheduling routes to the current worker or to the installed default pool — with neither, it asserts.
/// Binding a singlethreaded_scheduler here gives the whole graph, actor included, somewhere to run.

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

    /// Every storage error the cache reported, in order.
    /// Empty is the normal case.
    [[nodiscard]] cc::span<cc::string const> errors() const { return _errors; }

    /// Closes and reopens over the same file, which is how the durability tests get a second process' worth of separation without spawning one.
    void reopen(cc::function_ref<void(cache_config&)> configure = [](cache_config&) {});

    /// Opens a SECOND cache over the same file: two connections, which is the multi-writer property without a second process.
    ///
    /// The returned cache is registered with this fixture, so drive_until pumps it too — an unthreaded actor
    /// nobody pumps simply never services its mailbox, and every wait on it would time out.
    [[nodiscard]] cc::unique_ptr<blob_cache> open_second();

    /// Drives the actor and the async graph until `node` resolves.
    /// FAILs rather than spinning forever, so a pipeline that can never complete is a failing test and not a hang.
    template <class T>
    T settle(cc::shared_async<T> const& node)
    {
        this->drive_until([&] { return node->is_ready(); });
        return this->take(node);
    }

    /// settle() for a node whose value is not wanted.
    template <class T>
    void settle_only(cc::shared_async<T> const& node)
    {
        this->drive_until([&] { return node->is_ready(); });
    }

    /// Pumps the actor and the scheduler until `done`, or fails the test.
    void drive_until(cc::function_ref<bool()> done);

    /// Drives until nothing moves, without requiring anything to finish — for "nothing further happened" checks.
    /// Quiescence rather than a cycle count: a sibling test sweeping the same registry can hold this store's pump, and
    /// a skipped cycle is not one this store got.
    void idle();

private:
    template <class T>
    T take(cc::shared_async<T> const& node)
    {
        if (auto const* v = node->try_value())
            return *v;
        return T();
    }

    struct driver;

    cc::string _path;
    std::shared_ptr<fake_clock> _clock;
    std::shared_ptr<cc::vector<cc::string>> _reported;
    cc::vector<cc::string> _errors;
    cc::unique_ptr<blob_cache> _cache;
    cc::unique_ptr<driver> _driver;
};

namespace bcache::test
{

/// A blob over `text`, owning its own copy.
[[nodiscard]] blob make_blob(cc::string_view text);

/// A blob of `size` bytes, filled from `seed` so two different seeds never collide and one seed always repeats.
[[nodiscard]] blob make_blob_of_size(isize size, u8 seed);

[[nodiscard]] cc::string blob_text(blob const& b);

[[nodiscard]] cache_key key_of(cc::string_view space, cc::string_view key, i32 version = 1);
} // namespace bcache::test
