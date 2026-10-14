#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/string/format.hh>
#include <nexus/run.hh>

int main(int argc, char** argv)
{
    // Every compiler edge a test builds keeps its persistent tier in bcache::default_cache() unless it is handed a store.
    // So the binary installs one of its own: no test reads or writes the user's cache, and every run starts empty.
    // Without threads a store's actor is a pump, still registered when nexus checks for leaked ones, so there it has no storage at all.
    auto const path = cc::temp_file_path("slib-test-cache", ".db");
    auto const cache
        = CC_HAS_THREADS ? bcache::blob_cache::create({.path = path}) : bcache::blob_cache::create_disabled();
    auto const installed = bcache::scoped_default_cache(cache.get());

    auto const result = nx::run(argc, argv);

    // Closed while still installed, so nothing late reaches the user's cache; the WAL and shm siblings go with the file.
    cache->close();
    (void)cc::remove_file(path);
    (void)cc::remove_file(cc::format("{}-wal", path));
    (void)cc::remove_file(cc::format("{}-shm", path));
    return result;
}
