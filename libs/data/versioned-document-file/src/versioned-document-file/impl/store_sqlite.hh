#pragma once

#include <versioned-document-file/impl/sqlite_actor.hh>
#include <versioned-document-file/store.hh>

/// The SQLite-backed store: the seam's file arm.
///
/// It holds an ACTOR, not a connection.
/// Every hook makes a manual async, enqueues a message, and pumps; the actor pushes the value or the error back from
/// its own thread.
/// A rejected enqueue — the actor is shutting down — resolves the async with an error immediately, which is what makes
/// publishing after close() fail fast rather than hang.

namespace vdoc::file::impl
{
class sqlite_store final : public store
{
public:
    sqlite_store() : _actor(make_sqlite_actor()) {}

    ~sqlite_store() override { close(); }

    /// The actor's mailbox, for the open message that fills this store.
    [[nodiscard]] sqlite_actor& actor() { return *_actor; }

    /// Drives pending storage work where the build has no threads; public here so store::open can pump before it returns.
    using store::impl_pump_until_idle;

protected:
    [[nodiscard]] cc::shared_async<publish_result> on_publish(publish_job job) override;
    [[nodiscard]] cc::shared_async<cc::vector<byte>> on_fetch_blob(blob_hash const& hash, blob_fetch_range range) override;
    [[nodiscard]] cc::shared_async<reclaim_result> on_reclaim(reclaim_job job) override;
    [[nodiscard]] cc::shared_async<cc::unit> on_flush_workspace(cc::vector<workspace_entry> entries) override;
    void on_close() override;
    [[nodiscard]] bool on_pump() override;

private:
    cc::unique_ptr<sqlite_actor> _actor;
};
} // namespace vdoc::file::impl
