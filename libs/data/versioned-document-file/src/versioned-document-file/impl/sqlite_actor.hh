#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/store.hh>

/// The actor that owns the connection.
///
/// **The database handle is a member of the actor implementation, not of the store.**
/// There is no path from a sqlite_store to a babel::sqlite::database — only a mailbox — so exclusive ownership is a
/// fact of the structure rather than a convention somebody could break by adding one accessor.
///
/// Every message type below is a pure vdoc type, so nothing sqlite ever crosses the mailbox either.
/// Results come back as push-based cc::async values: no caller ever blocks on storage, and no lock is held across a read.

namespace vdoc::file::impl
{
class sqlite_store;

/// Open, configure, load and verify.
/// The promise carries the finished handle, or the hard failure that stopped the open.
struct open_request
{
    cc::string path;

    /// BORROWED, never owned.
    ///
    /// The caller holds the handle from the instant open() returns, so this pointer is valid until close(), and the
    /// actor can never be the one that destroys the store — which would be a join against its own thread.
    /// Filling the store's plain members in place is safe because nothing else may touch it before `promise` resolves.
    sqlite_store* store = nullptr;

    cc::shared_async<cc::unit> promise;
};

struct publish_request
{
    publish_job job;
    cc::shared_async<publish_result> promise;
};

struct reclaim_request
{
    reclaim_job job;
    cc::shared_async<reclaim_result> promise;
};

struct workspace_request
{
    cc::vector<workspace_entry> entries;
    cc::shared_async<cc::unit> promise;
};

/// Closes the connection.
/// Sent by on_close, after close() has already flushed and drained.
struct close_request
{
};

/// One blob fetch: which blob, which range of its decoded bytes, and where the answer goes.
///
/// **Its position in the mailbox is load-bearing.** Messages dispatch in send order, so a fetch enqueued after a
/// publish sees that publish's writes — and no fetch ever interleaves with one, which is what makes an open blob
/// handle here impossible for this store to invalidate.
struct blob_request
{
    blob_hash hash;
    blob_fetch_range range;
    cc::shared_async<cc::vector<byte>> promise;
};

using sqlite_actor
    = cc::threaded_actor<open_request, publish_request, reclaim_request, workspace_request, blob_request, close_request>;

/// Creates and starts the actor.
/// Nothing touches the disk here — the open is a message like any other.
[[nodiscard]] cc::unique_ptr<sqlite_actor> make_sqlite_actor();
} // namespace vdoc::file::impl
