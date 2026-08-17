#pragma once

#include <blob-cache/blob_cache.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/threaded_actor.hh>

#include <memory> // std::weak_ptr — the actor reports counters into the core without owning it

/// The actor that owns the connection.
///
/// **The database handle is a member of the actor implementation, not of the cache.**
/// There is no path from a blob_cache to a babel::sqlite::database — only a mailbox — so exclusive ownership is a fact
/// of the structure rather than a convention somebody could break by adding one accessor.
///
/// Every message type below is a pure bcache type, so nothing sqlite ever crosses the mailbox either.
/// Results come back as push-based cc::async values: no caller ever blocks on storage, and no lock is held across a read.

namespace bcache::impl
{
struct cache_core;

/// Open, configure, check the schema, and seed the size accounting.
/// The promise carries nothing on success, and on failure the one line saying why storage is unavailable.
struct open_request
{
    cache_config config;

    /// BORROWED, never owned: the actor reporting counters into the core must not be what keeps the core alive, which would be a join against its own thread.
    std::weak_ptr<cache_core> core;

    cc::shared_async<cc::unit> promise;
};

/// One lookup.
///
/// **Its position in the mailbox is load-bearing.** Messages dispatch in send order, so a get enqueued after a put
/// sees that put's writes — and no get ever interleaves with one, which is what makes the blob handle a read streams through impossible for this actor to invalidate under itself.
struct get_request
{
    cache_key key;
    cc::shared_async<cc::optional<cache_hit>> promise;
};

struct put_request
{
    cache_key key;
    blob data;
    put_options options;
    cc::shared_async<put_result> promise;
};

struct invalidate_request
{
    cache_key key;
    cc::shared_async<bool> promise;
};

struct clear_request
{
    cache_namespace space;
    cc::shared_async<i64> promise;
};

/// New limits, applied from the next pass.
/// No promise: nothing waits on a limit change.
struct limits_request
{
    cache_limits limits;
};

/// Runs a pass to COMPLETION rather than one slice, which is what makes collect_garbage() a test's oracle.
struct gc_request
{
    cc::shared_async<gc_result> promise;
};

/// Writes the buffered access times out now.
/// What close() and a test both need.
struct flush_request
{
    cc::shared_async<cc::unit> promise;
};

/// Closes the connection.
/// Sent by close(), after the flush has been enqueued ahead of it.
struct close_request
{
};

using cache_actor
    = cc::threaded_actor<open_request, get_request, put_request, invalidate_request, clear_request, limits_request, gc_request, flush_request, close_request>;

/// Creates and starts the actor.
/// Nothing touches the disk here — the open is a message like any other, which is what lets create() return a usable handle without waiting on I/O.
[[nodiscard]] cc::unique_ptr<cache_actor> make_cache_actor(bool unthreaded);
} // namespace bcache::impl
