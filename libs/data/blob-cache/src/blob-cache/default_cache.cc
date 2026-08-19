#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <blob-cache/impl/cache_paths.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/atomic.hh>

namespace bcache
{
namespace
{
/// What set_default_cache installed, or null for "use the lazily-opened one".
/// Atomic because a library may read it from a worker while the main thread is still installing.
auto g_installed = cc::atomic<blob_cache*>(nullptr);

cc::string default_cache_directory()
{
    return cc::format("{}/shaped-core", impl::user_cache_directory());
}

blob_cache& lazy_default()
{
    // Function-local, so nothing opens at static-init time and the destructor's close() — which flushes buffered
    // access times and joins the actor — runs at exit rather than never.
    static auto const instance = []
    {
        auto const mode = cc::environment_variable(cache_mode_env_var);
        if (mode.has_value() && mode.value() == "off")
            return blob_cache::create_disabled();

        if (mode.has_value() && mode.value() == "temp")
            return blob_cache::create({.path = cc::temp_file_path("shaped-core-cache", ".db")});

        // A missing directory is not an error to blob_cache: it opens degraded and misses forever.
        // Which is exactly why this has to happen here, where the default path is chosen.
        (void)impl::create_directories(default_cache_directory());
        return blob_cache::create({.path = default_cache_path()});
    }();
    return *instance;
}

blob_cache& disabled_default()
{
    static auto const instance = blob_cache::create_disabled();
    return *instance;
}
} // namespace

cc::string default_cache_path()
{
    return cc::format("{}/blob-cache.db", default_cache_directory());
}

blob_cache& default_cache()
{
    if (auto* const installed = g_installed.load(); installed != nullptr)
        return *installed;
    return lazy_default();
}

void set_default_cache(blob_cache* cache)
{
    g_installed.store(cache);
}

void disable_default_cache()
{
    g_installed.store(&disabled_default());
}

scoped_default_cache::scoped_default_cache(blob_cache* cache) : _previous(g_installed.load())
{
    g_installed.store(cache);
}

scoped_default_cache::~scoped_default_cache()
{
    g_installed.store(_previous);
}
} // namespace bcache
