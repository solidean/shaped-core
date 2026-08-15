#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <versioned-document-file/assets.hh>
#include <versioned-document-file/diagnostics.hh>
#include <versioned-document-file/fwd.hh>
#include <versioned-document-file/impl/store_io.hh>
#include <versioned-document-file/workspace.hh>
#include <versioned-document/op_graph.hh>

#include <memory>

/// The store: one `.vdoc` document, however it is backed.
///
/// **The loaded state is plain members, filled once at load; only keeping it in sync with storage is virtual.**
/// That split is the whole seam — the two implementations share every query, every reachability computation and every
/// diagnostic, and differ in four hooks and nothing else.
///
/// **One thread owns a store.** What makes the API non-blocking is that storage work runs on an actor, not that
/// several threads may call in.
///
/// The design is [format.md](../../docs/format.md), and the milestone is
/// [milestone-4](../../../versioned-document/docs/todo/milestone-4.md).

namespace vdoc::file
{
/// An owning handle to a store.
///
/// Shared because a blob_source outlives the caller's own reference, and std::shared_ptr rather than cc::shared_ptr
/// because the handle is POLYMORPHIC: cc::default_shared_traits places its control block at an offset derived from
/// sizeof(T) of the static type, which a derived store would land inside.
/// The same clean-core gap shaped-graphics and shaped-shader-library name, and the one entry in our .shaped-lint.yml.
using store_handle = std::shared_ptr<store>;

} // namespace vdoc::file

/// The two halves of an open: the store, and when its load finished.
///
/// They are separate because the CALLER must own the store from the first instant.
/// The actor may only ever hold a borrowed pointer to it: an actor that held the last reference would destroy the store
/// — and with it the actor — on the actor's own thread, which is a join against itself.
/// Handing the handle back synchronously makes that impossible by construction rather than by care.
struct vdoc::file::open_result
{
    /// Usable immediately, and EMPTY until `loaded` is ready.
    store_handle store;

    /// Ready once the load finished.
    /// A hard failure rides its error channel, and leaves the store empty.
    cc::shared_async<cc::unit> loaded;
};

/// A materialized document cached against an op, so loading need not replay history back to the root.
///
/// **Opaque in this milestone.** Nothing here decodes a snapshot: the row round-trips byte for byte, and `encoding` is
/// the seam a decoder attaches to in milestone 6.
/// `required` is carried rather than inferred, because a required snapshot that will not decode is a hard failure
/// while a droppable one is an issue.
struct vdoc::file::snapshot_entry
{
    vdoc::op_id op;
    /// True where history behind this op has been pruned, so deleting the row destroys data.
    bool required = false;
    cc::string encoding;
    cc::vector<byte> data;

    /// Spelled out rather than defaulted, because cc::vector carries no equality of its own.
    /// Byte equality on `data` is the only equality an opaque payload can have.
    [[nodiscard]] friend bool operator==(snapshot_entry const& a, snapshot_entry const& b)
    {
        if (a.op != b.op || a.required != b.required || a.encoding != b.encoding || a.data.size() != b.data.size())
            return false;
        for (isize i = 0; i < a.data.size(); ++i)
            if (a.data[i] != b.data[i])
                return false;
        return true;
    }
};

/// What a caller asks to publish: ref moves plus the assets and blobs that go with them.
///
/// **Ops are not listed** — the store derives them from the refs by reachability.
/// That is a safety property rather than a convenience: an op no ref can reach — an abandoned branch, a discarded drag
/// preview — cannot be published by mistake, even by a caller who wanted to.
/// Assets and blobs must be listed, because the store cannot know about bytes sitting in memory.
struct vdoc::file::publish_changes
{
    cc::vector<cc::pair<cc::string, vdoc::op_id>> refs;
    cc::vector<asset_record> assets;
    cc::vector<blob_upload> blobs;
};

/// What a publish actually had to write.
/// Both are zero for a publish that was already durable, which is what idempotence looks like from the outside.
struct vdoc::file::publish_result
{
    isize ops_written = 0;
    isize blobs_written = 0;

    [[nodiscard]] friend bool operator==(publish_result const& a, publish_result const& b) = default;
};

/// Fetches asset blob bytes on demand.
///
/// Handed to whatever resolves assets for the application; the store itself never interprets a blob.
/// Holding one keeps the storage alive, and close() severs it — after which load() completes with an error rather than
/// hanging on a dead handle.
class vdoc::file::blob_source
{
public:
    /// The decoded bytes of `hash`.
    ///
    /// **Enqueue-and-return.** This never blocks and never re-enters its caller, whatever state the store is in.
    /// The live fetch path is milestone 5; until then a live source reports that, and a severed one reports being severed.
    [[nodiscard]] cc::shared_async<cc::vector<byte>> load(blob_hash const& hash);

    /// True once the store that made this closed.
    [[nodiscard]] bool is_severed() const { return _severed; }

private:
    friend class store;
    explicit blob_source(store_handle owner) : _owner(cc::move(owner)) {}

    void impl_sever();

    store_handle _owner;
    bool _severed = false;
};

class vdoc::file::store : public std::enable_shared_from_this<store>
{
    // opening
public:
    /// Opens a `.vdoc` file, creating it if the path does not exist.
    ///
    /// Returns at once, having touched no disk: opening, checking the schema and loading all run on the store's own actor.
    /// The store comes back immediately and is empty until `open_result::loaded` is ready.
    /// A hard failure rides that async's error channel; everything else is a load issue and the store opens anyway.
    [[nodiscard]] static open_result open(cc::string_view path);

    /// Whether file-backed storage was compiled in at all.
    ///
    /// A runtime probe, never a macro: false makes open() report the absence rather than making any declaration
    /// disappear, and it is what a test SKIPs on.
    [[nodiscard]] static bool is_file_storage_available();

    /// An unsaved new document, over an image it owns alone.
    /// Its state dies with it, which is what "unsaved" means.
    [[nodiscard]] static store_handle create_in_memory();

    /// An in-memory store over an image the caller owns and outlives it.
    ///
    /// Reopening one re-runs the load — its decoding, its verification and its issues — which is what makes the
    /// in-memory arm an oracle rather than a shortcut.
    [[nodiscard]] static store_handle create_in_memory(std::shared_ptr<memory_image> image);

    // the loaded document
public:
    /// Loaded and verified in full, eagerly, at open.
    [[nodiscard]] vdoc::op_graph const& ops() const { return _ops; }

    /// The named heads, kept verbatim — including one whose op this load dropped, which is reported as a dangling ref.
    [[nodiscard]] cc::map<cc::string, vdoc::op_id> const& refs() const { return _refs; }

    [[nodiscard]] cc::map<vdoc::op_id, snapshot_entry> const& snapshots() const { return _snapshots; }
    [[nodiscard]] cc::map<cc::string, asset_record> const& assets() const { return _assets; }

    /// File-level facts rather than document ones — the writer's build, creation time.
    /// Keys this build does not know are preserved by never being rewritten.
    [[nodiscard]] cc::map<cc::string, vdoc::value> const& meta() const { return _meta; }

    /// The soft failures this load found.
    /// A hard failure never reaches here: it rides the open's failure channel instead.
    [[nodiscard]] load_report const& report() const { return _report; }

    /// Adds a newly built op to the document.
    ///
    /// It becomes reachable — and therefore publishable — only once a ref is moved onto it, which is the whole point
    /// of deriving the op set from the refs.
    /// Idempotent, since the graph is keyed by content hash.
    vdoc::op_id add_op(vdoc::op op) { return _ops.add(cc::move(op)); }

    // publishing
public:
    /// Publishes ref moves plus the assets and blobs that go with them; the ops follow from the refs by reachability.
    ///
    /// **Fire-and-forget.** Hold the async and wait at save or close, or read sticky_error() to see a failure that
    /// already happened.
    /// The refs move at enqueue time, which is what is_saved reports on.
    [[nodiscard]] cc::shared_async<publish_result> publish(publish_changes changes);

    /// Would publishing `head` be a no-op? — what drives "nothing to save".
    ///
    /// **Queued, not committed.** A publish that later failed un-claims its ops here and surfaces in sticky_error().
    [[nodiscard]] bool is_saved(vdoc::op_id const& head);

    /// The FIRST publish failure, or null.
    ///
    /// Harvests finished publishes on the way, which is why it is not const — and why a failing autosave is visible at
    /// the next call rather than only at close.
    /// A workspace flush failure is deliberately never latched here.
    [[nodiscard]] cc::any_error const* sticky_error();

    /// A source for this store's blob bytes, kept alive by the handle it holds.
    [[nodiscard]] std::shared_ptr<blob_source> make_blob_source();

    // the workspace
public:
    /// Marks `key` dirty and performs no I/O, so it is safe to call every frame.
    /// Creates no op, moves no ref, and never affects is_saved.
    void set_workspace(cc::string_view key, workspace_value value);

    /// The stored value for `key`, or empty when it is absent OR stored under a version other than `version`.
    ///
    /// The store cannot know an application's versions, so the caller names the one it can handle.
    /// A row under any other version reads as absent and is left in the table, because only dirty keys are ever written.
    [[nodiscard]] cc::optional<vdoc::value_view> try_get_workspace(cc::string_view key, i32 version) const;

    [[nodiscard]] cc::map<cc::string, workspace_value> const& workspace() const { return _workspace; }

    /// Writes only the dirty keys, which is what keeps a newer build's keys unclobbered.
    /// A failure is deliberately NOT latched: losing a camera position is not the data loss the latch exists to report.
    [[nodiscard]] cc::shared_async<cc::unit> flush_workspace();

    // closing
public:
    /// Flushes the workspace, drains accepted publishes, rejects new ones, severs every blob source, and releases storage.
    ///
    /// **Non-virtual on purpose**: no implementation gets to write this, so none can forget the flush.
    /// Idempotent, and the destructor calls it.
    void close();

    [[nodiscard]] bool is_closed() const { return _is_closed; }

    virtual ~store();

    store(store const&) = delete;
    store& operator=(store const&) = delete;

    // the seam — four hooks, and nothing else differs
protected:
    store() = default;

    /// Persists one already-computed publish.
    [[nodiscard]] virtual cc::shared_async<publish_result> on_publish(impl::publish_job job) = 0;

    /// Persists the dirty workspace entries.
    [[nodiscard]] virtual cc::shared_async<cc::unit> on_flush_workspace(cc::vector<workspace_entry> entries) = 0;

    /// Releases storage, after close() has flushed and drained.
    /// Runs exactly once.
    virtual void on_close() = 0;

    /// Runs pending storage work on the calling thread where the build has no threads; false when nothing is left.
    ///
    /// A no-op returning false in a threaded build, so every caller calls it unconditionally.
    /// That is how CC_HAS_THREADS is honoured without a single #if anywhere in this library.
    [[nodiscard]] virtual bool on_pump() { return false; }

    // the loaded state — plain members, filled once at load
protected:
    friend cc::result<cc::unit> impl::load(impl::store_reader& reader, store& target);

    vdoc::op_graph _ops;
    cc::map<cc::string, vdoc::op_id> _refs;
    cc::map<vdoc::op_id, snapshot_entry> _snapshots;
    cc::map<cc::string, asset_record> _assets;
    cc::map<cc::string, workspace_value> _workspace;
    cc::map<cc::string, vdoc::value> _meta;
    load_report _report;

    /// Every op believed to be in storage — the delta a publish writes is `reachable − this`.
    ///
    /// **A pure optimization.** Publishing is idempotent, so a set that is too small costs a rewrite and never
    /// correctness — which is why a failed publish un-claims what it had added.
    cc::set<vdoc::op_id> _durable_ops;
    cc::set<blob_hash> _durable_blobs;

    /// Drives on_pump() until it reports nothing left.
    /// Threaded, the first call already returns false and the asyncs stay pending; unthreaded, the work has run by then.
    void impl_pump_until_idle();

    // the non-virtual machinery
private:
    /// Collects finished publishes, latches the first failure, and un-claims a failed job's ops.
    void impl_harvest_pending();

    struct pending_publish
    {
        cc::shared_async<publish_result> async;
        /// Un-claimed from _durable_ops if this publish fails, so a retry writes them again.
        cc::vector<vdoc::op_id> claimed;
        cc::vector<blob_hash> claimed_blobs;
    };

    cc::vector<pending_publish> _pending;
    /// Drained at close, and never looked at by the latch.
    cc::vector<cc::shared_async<cc::unit>> _pending_workspace;
    cc::set<cc::string> _workspace_dirty;
    cc::optional<cc::any_error> _sticky_error;
    cc::vector<std::weak_ptr<blob_source>> _blob_sources;
    bool _is_closed = false;
};
