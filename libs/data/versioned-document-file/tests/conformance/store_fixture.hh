#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <versioned-document-file/memory_image.hh>
#include <versioned-document-file/store.hh>
#include <versioned-document/op_graph.hh>

#include <memory>

/// The conformance suite's fixture: one set of tests, parametrized over both store implementations.
///
/// **The in-memory arm is the oracle, and it is held to the same tests.**
/// It writes into a memory_image that outlives the store, so close-and-reopen means the same thing on both arms — the load runs again,
/// with its decoding, its verification and its issues.
/// A test that only one arm could pass would not be a conformance test.

namespace vdoc::file::test
{
class store_medium;
struct store_impl;
struct sample_history;
} // namespace vdoc::file::test

/// Where one test's document lives, for as long as that test runs.
///
/// RAII: a file arm removes its temp file, an in-memory arm drops its image.
/// Everything a test needs to do to a store *from outside* is here, because damaging a document on purpose is the one thing a store's own API must never offer.
class vdoc::file::test::store_medium
{
public:
    virtual ~store_medium() = default;

    store_medium() = default;
    store_medium(store_medium const&) = delete;
    store_medium& operator=(store_medium const&) = delete;

    /// Opens the medium, running a full load.
    /// Empty where the open failed hard, which is what a future user_version or a damaged header produces.
    [[nodiscard]] virtual cc::optional<store_handle> open() = 0;

    /// The medium's whole content, for a byte-identical comparison across a publish that should have changed nothing.
    [[nodiscard]] virtual cc::vector<byte> snapshot_bytes() = 0;

    /// Stamps a format version, so a future one can be opened and refused.
    virtual void set_user_version(i32 version) = 0;

    /// Adds state a newer build might have written, which this one must ignore and preserve.
    virtual void add_unknown_table(cc::string_view name) = 0;
    virtual void add_unknown_column(cc::string_view table, cc::string_view column) = 0;

    /// Flips one byte inside a stored op's payload, so it no longer hashes to its id.
    /// True if there was an op to damage.
    virtual bool corrupt_first_op_payload() = 0;

    /// Replaces a stored op's payload with bytes that were never an op.
    virtual bool corrupt_first_op_structurally() = 0;

    /// Makes every subsequent write fail, so a failing autosave can be observed.
    virtual void block_writes() = 0;
    virtual void unblock_writes() = 0;

    /// Deletes one chunk of the lowest-id blob, behind the store's back — a torn blob.
    /// True if there was a chunk to delete.
    virtual bool delete_first_blob_chunk() = 0;

    /// Rewrites the lowest-id blob's encoding, so the file names one this build has no codec for.
    virtual bool set_first_blob_encoding(cc::string_view encoding) = 0;

    /// Replaces the first asset's `deps` column with bytes that were never a vdoc value.
    /// True if there was an asset carrying one.
    virtual bool corrupt_first_asset_deps() = 0;

    /// How many blobs are stored, which is what a dedup assertion counts.
    [[nodiscard]] virtual isize count_blobs() = 0;

    /// How many snapshot rows are stored, whatever their state.
    [[nodiscard]] virtual isize count_snapshots() = 0;

    /// Deletes the lowest-keyed snapshot row and its chunks, behind the store's back.
    /// True if there was one.
    virtual bool delete_first_snapshot() = 0;

    /// Replaces the lowest-keyed snapshot's payload with bytes that were never a snapshot.
    /// True if there was one.
    virtual bool corrupt_first_snapshot_payload() = 0;

    /// Flips the lowest-keyed snapshot's `required` flag, so both severities are reachable from one fixture.
    virtual bool set_first_snapshot_required(bool required) = 0;

    /// Rewrites the lowest-keyed snapshot's encoding, so the file names one this build has no codec for.
    virtual bool set_first_snapshot_encoding(cc::string_view encoding) = 0;

    /// Whether the lowest-keyed snapshot is marked required — read back from storage rather than from the store.
    [[nodiscard]] virtual bool first_snapshot_is_required() = 0;
};

/// One store implementation, as the conformance suite drives it.
///
/// **Stateless on purpose.**
/// nexus boxes an invocable's arguments by decayed value and shares one box across every matched test, so per-test state cannot live here —
/// the medium is made from this, and never carried in it.
struct vdoc::file::test::store_impl
{
    cc::string_view name;
    std::unique_ptr<store_medium> (*make_medium)();
    /// False where the backend was not compiled in, which is what the driver SKIPs on.
    bool (*is_available)();
};

/// A three-op linear history over one entity, plus the head it ends at.
struct vdoc::file::test::sample_history
{
    vdoc::op_graph graph;
    cc::vector<vdoc::op_id> ops;

    [[nodiscard]] vdoc::op_id head() const { return ops.back(); }
};

namespace vdoc::file::test
{
/// The in-memory arm.
[[nodiscard]] store_impl in_memory_impl();

/// The SQLite arm, over a real file in the OS temp directory.
[[nodiscard]] store_impl sqlite_impl();

/// Waits for `async` and moves the outcome out.
///
/// Unthreaded it is already resolved on return from the call that made it, because every entry point pumps before handing one back;
/// threaded the actor resolves it and this spins.
/// No test may assume either, which is what makes the suite pass under both SC_THREADS settings.
template <class T>
[[nodiscard]] cc::result<T, cc::async_error> wait_for(cc::shared_async<T> async)
{
    while (!async->is_ready())
    {
    }
    return cc::into_result(cc::move(async));
}

/// Waits for `async`, pumping `s` while it waits.
///
/// A blob fetch needs this: the in-memory arm has no thread of its own, so its fetches complete inside pump().
/// The file arm's actor has usually resolved by then and the pump is a no-op, which is why one form serves both.
template <class T>
[[nodiscard]] cc::result<T, cc::async_error> wait_for(store& s, cc::shared_async<T> async)
{
    while (!async->is_ready())
        (void)s.pump();
    return cc::into_result(cc::move(async));
}

/// Builds a small history with real assignments, so a round-trip compares something.
[[nodiscard]] sample_history make_sample_history();

/// A linear history of `length` ops over one entity, each overwriting the last.
/// Long enough that pruning it leaves something behind, which the three-op sample is not.
[[nodiscard]] sample_history make_linear_history(isize length);

/// A second head branching off `after`, reachable from no ref unless one is published for it.
[[nodiscard]] vdoc::op_id add_branch(vdoc::op_graph& graph, vdoc::op_id const& after, cc::string_view marker);

/// Copies the named ops out of `source` into `target`, which is what a caller does before publishing them.
void copy_ops_into(store& target, vdoc::op_graph const& source, cc::span<vdoc::op_id const> ids);

/// The materialized document as a comparable string, so two loads can be checked against each other.
[[nodiscard]] cc::string materialize_to_text(vdoc::op_graph const& graph, vdoc::op_id const& head);
} // namespace vdoc::file::test
