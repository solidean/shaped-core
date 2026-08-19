#include "cache_fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/thread_pump.hh>
#include <nexus/test.hh>

namespace bcache::test
{
namespace
{
/// A file name nobody else in this run will pick.
/// A counter rather than a fixed name: several fixtures live at once inside one test binary.
cc::string unique_cache_path()
{
    static cc::atomic<i64> counter = {0};
    return cc::format("{}/bcache-test-{}.db", cc::temp_directory_path(), counter.fetch_add(1, cc::memory_order_relaxed));
}

/// A database leaves two siblings behind, and a stale WAL beside a fresh file is a real corruption source —
/// so a fixture that removed only the file would poison the next run that happened to pick the same name.
void remove_database(cc::string_view path)
{
    cc::remove_file(path);
    cc::remove_file(cc::format("{}-wal", path));
    cc::remove_file(cc::format("{}-shm", path));
}
} // namespace

/// The scheduler this fixture binds to the calling thread, and the scope that binds it.
///
/// Declared out of line so the header does not have to name the scheduler types.
struct cache_fixture::driver
{
    cc::singlethreaded_scheduler scheduler;
    cc::async_worker_scope scope = cc::async_worker_scope(scheduler);
};

cache_fixture::cache_fixture(cc::function_ref<void(cache_config&)> configure)
  : _path(unique_cache_path()),
    _clock(std::make_shared<fake_clock>()),
    _reported(std::make_shared<cc::vector<cc::string>>())
{
    remove_database(_path); // a leftover from a crashed run must not decide this test
    _driver = cc::make_unique<driver>();
    this->reopen(configure);
}

cache_fixture::~cache_fixture()
{
    _cache = nullptr; // closes and joins while the scheduler is still bound
    _driver = nullptr;
    remove_database(_path);
}

void cache_fixture::reopen(cc::function_ref<void(cache_config&)> configure)
{
    _cache = nullptr;

    auto config = cache_config{.path = _path, .unthreaded = true};
    config.wall_clock = [clock = _clock] { return clock->now(); };
    config.steady_clock = [clock = _clock] { return clock->now(); };
    config.on_storage_error
        = [reported = _reported](cc::string_view message) { reported->push_back(cc::string(message)); };
    configure(config);

    _cache = blob_cache::create(cc::move(config));
    this->settle_only(_cache->opened());

    for (auto const& e : *_reported)
        _errors.push_back(e);
    _reported->clear();
}

cc::unique_ptr<blob_cache> cache_fixture::open_second()
{
    auto config = cache_config{.path = _path, .unthreaded = true};
    config.wall_clock = [clock = _clock] { return clock->now(); };
    config.steady_clock = [clock = _clock] { return clock->now(); };

    auto second = blob_cache::create(cc::move(config));
    _also_driven.push_back(second.get());
    return second;
}

void cache_fixture::drive_until(cc::function_ref<bool()> done)
{
    // Generous, because one acquire is several actor round trips and a GC pass is many.
    // Small enough that a pipeline which can never finish fails in well under a second rather than hanging the suite.
    constexpr auto max_cycles = 100000;

    for (auto i = 0; i < max_cycles; ++i)
    {
        if (done())
            return;

        // The actors first: they are what resolve the promises the graph is parked on.
        // This test's own stores by name, never cc::thread_pump_all(): a global sweep also runs the stores of every
        // sibling test running beside this one, at moments they did not choose.
        (void)_cache->pump();
        for (auto* other : _also_driven)
            (void)other->pump();
        _driver->scheduler.drain();
    }

    CHECK(false); // "settle" never settled — a step is missing, not merely slow
}

void cache_fixture::idle(int cycles)
{
    for (auto i = 0; i < cycles; ++i)
    {
        (void)_cache->pump();
        for (auto* other : _also_driven)
            (void)other->pump();
        _driver->scheduler.drain();
    }
}

blob make_blob(cc::string_view text)
{
    auto data = cc::pinned_data<byte>::create_uninitialized(text.size());
    cc::memcpy(data.data(), text.data(), size_t(text.size()));
    return blob(data);
}

blob make_blob_of_size(isize size, u8 seed)
{
    auto data = cc::pinned_data<byte>::create_uninitialized(size);
    for (auto i = isize(0); i < size; ++i)
        data[i] = byte(u8(seed + u8(i * 31)));
    return blob(data);
}

cc::string blob_text(blob const& b)
{
    return cc::string(cc::string_view(reinterpret_cast<char const*>(b.data()), b.size()));
}

cache_key key_of(cc::string_view space, cc::string_view key, i32 version)
{
    return {.space = cache_namespace(space),
            .key = logical_key::create_from_string(key),
            .version = bcache::version(version)};
}
} // namespace bcache::test
