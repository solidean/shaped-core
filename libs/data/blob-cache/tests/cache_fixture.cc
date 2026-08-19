#include "cache_fixture.hh"

#include <clean-core/common/time.hh>
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

    return blob_cache::create(cc::move(config));
}

void cache_fixture::drive_until(cc::function_ref<bool()> done)
{
    // Bounded by TIME, not by cycles: a sibling test sweeping the same registry holds this store's pump while it runs
    // it, and our sweep skips a pump already running.
    // Counting those skips as attempts would give up while somebody else was making the very progress we wait for.
    // Generous, because one acquire is several actor round trips and a GC pass is many.
    auto const deadline = cc::current_time_steady_secs() + 5.0;

    while (cc::current_time_steady_secs() < deadline)
    {
        if (done())
            return;

        // The actors first: they are what resolve the promises the graph is parked on.
        // Through the registry, never store by store: driving one by name would test a local pump and leave the real
        // mechanism — an unthreaded store registering itself — broken and unnoticed.
        (void)cc::thread_pump_all();
        _driver->scheduler.drain();
    }

    CHECK(done()); // "settle" never settled — a step is missing, not merely slow
}

void cache_fixture::idle()
{
    // Until nothing moves, rather than a fixed number of cycles, for the same reason drive_until is bounded by time:
    // a sibling test sweeping the same registry holds this store's pump while it runs it, and a cycle that skipped a
    // busy pump is not a cycle this store got.
    // Quiescence is also the stronger claim — "everything that could happen has" rather than "four tries' worth".
    auto const deadline = cc::current_time_steady_secs() + 5.0;

    while (cc::thread_pump_all() && cc::current_time_steady_secs() < deadline)
        _driver->scheduler.drain();

    _driver->scheduler.drain(); // the last sweep's completions still have to be resumed
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
