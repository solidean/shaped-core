#include "cache_fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>

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

cache_fixture::cache_fixture(cc::function_ref<void(cache_config&)> configure)
  : _path(unique_cache_path()),
    _clock(std::make_shared<fake_clock>()),
    _reported(std::make_shared<cc::vector<cc::string>>())
{
    remove_database(_path); // a leftover from a crashed run must not decide this test
    this->reopen(configure);
}

cache_fixture::~cache_fixture()
{
    _cache = nullptr; // closes and drains its mailbox on this thread before the file goes
    remove_database(_path);
}

void cache_fixture::reopen(cc::function_ref<void(cache_config&)> configure)
{
    _cache = nullptr;

    auto config = cache_config{.path = _path, .unthreaded = false};
    config.wall_clock = [clock = _clock] { return clock->now(); };
    config.steady_clock = [clock = _clock] { return clock->now(); };
    config.on_storage_error
        = [reported = _reported](cc::string_view message) { reported->push_back(cc::string(message)); };

    // No automatic GC pass unless a test asks for one.
    // The actor processes whenever it has work, so a pass the clock makes due can run between a test's advance and
    // its own collect_garbage, and take the expiries that call was meant to count.
    config.gc_interval_secs = 1e9;
    configure(config);

    // Not awaited: the open is the first message in the mailbox, so everything a test sends is handled after it.
    _cache = blob_cache::create(cc::move(config));
}

cc::unique_ptr<blob_cache> cache_fixture::open_second()
{
    auto config = cache_config{.path = _path, .unthreaded = false};
    config.wall_clock = [clock = _clock] { return clock->now(); };
    config.steady_clock = [clock = _clock] { return clock->now(); };

    return blob_cache::create(cc::move(config));
}

blob make_blob(cc::string_view text)
{
    auto data = cc::pinned_data<byte>::create_uninitialized(text.size());

    // Guarded because memcpy's pointers must be non-null even when the count is 0, and a zero-size pinned_data owns no buffer to point at.
    // `make_blob("")` is a case the suite deliberately exercises — an empty payload is a value the cache must store rather than an input to reject — so this is reached and not merely possible.
    if (!text.empty())
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
