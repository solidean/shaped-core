#pragma once

#include <blob-cache/fwd.hh>
#include <clean-core/string/string.hh>

/// The process-wide cache every subsystem shares.
///
/// One big cache beats several small ones: a shared budget lets a cold shader compile evict a stale texture mip, which
/// per-subsystem caches can never do because each one only ever evicts its own.
/// So nothing here is per-library, and a library that caches reaches for default_cache() rather than asking its caller for one.

namespace bcache
{
/// Where default_cache() opens, when nothing was installed over it: `<user cache dir>/shaped-core/blob-cache.db`.
/// %LOCALAPPDATA% on Windows, $XDG_CACHE_HOME or ~/.cache on Linux, ~/Library/Caches on Apple platforms.
[[nodiscard]] cc::string default_cache_path();

/// The cache to use when the caller was given none.
///
/// Opened on the first call and never at static-init time, creating its directory if it is missing.
/// Never fails: a directory that cannot be made, or a build with no SQLite backend, hands back a cache that misses on
/// everything — which is what makes calling this unconditionally correct.
[[nodiscard]] blob_cache& default_cache();

/// Installs `cache` as what default_cache() returns; nullptr restores the lazily-opened one.
///
/// The caller keeps ownership and must outlive every use — including any acquire still in flight.
/// Install before the first cached work rather than mid-run: callers are free to hold the reference default_cache() gave them.
void set_default_cache(blob_cache* cache);

/// Makes the default a cache with no storage: every get misses and every put is dropped, while acquire still singleflights.
/// How a process turns persistence off without branching anywhere.
void disable_default_cache();
} // namespace bcache

/// Installs a default for a scope and restores the previous one after.
///
/// What a test binary uses, and it is not optional there: without it a test writes into the developer's real cache
/// directory, where its entries outlive the run and its keys collide with a neighbouring binary's.
struct bcache::scoped_default_cache
{
    explicit scoped_default_cache(blob_cache* cache);
    ~scoped_default_cache();

    scoped_default_cache(scoped_default_cache const&) = delete;
    scoped_default_cache(scoped_default_cache&&) = delete;
    scoped_default_cache& operator=(scoped_default_cache const&) = delete;
    scoped_default_cache& operator=(scoped_default_cache&&) = delete;

private:
    blob_cache* _previous = nullptr;
};
