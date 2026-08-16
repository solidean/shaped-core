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
#include <versioned-document/recovery.hh>
#include <versioned-document/snapshot_cache.hh>

#include <memory>

/// The store: one `.vdoc` document, however it is backed.
///
/// **The loaded state is plain members, filled once at load; only keeping it in sync with storage is virtual.**
/// That split is the whole seam — the two implementations share every query, every reachability computation and every
/// diagnostic, and differ in seven hooks and nothing else.
///
/// **One thread owns a store.** What makes the API non-blocking is that storage work runs on an actor, not that
/// several threads may call in.
///
/// The design is [format.md](../../docs/format.md), and the model it stores is
/// [versioned-document](../../../versioned-document/docs/_index.md#concepts)'s.

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

/// What the file records about one snapshot — never its payload.
///
/// **The bytes are deliberately not here.** A decodable snapshot has already become an entry in the store's
/// snapshot_cache, and one this build cannot decode is bytes nobody can use; keeping either resident would mean
/// holding a second copy of something that can run to gigabytes.
/// A row this build cannot read still round-trips untouched, because publishing only ever inserts and never rewrites.
///
/// `required` is carried rather than inferred, because a required snapshot that will not decode is a hard failure
/// while a droppable one is an issue.
struct vdoc::file::snapshot_entry
{
    vdoc::op_id op;
    /// True where history behind this op has been pruned, so deleting the row destroys data.
    bool required = false;
    cc::string encoding;
    i64 decoded_size = 0;

    /// True where this build has a codec for `encoding` and the bytes decoded, so the cache has it.
    bool decoded = false;

    [[nodiscard]] friend bool operator==(snapshot_entry const&, snapshot_entry const&) = default;
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

    /// Asset ids to unmap, applied after the upserts.
    ///
    /// Retroactive exactly like a remap: it changes what every past version of the document resolves to, creates no op
    /// and moves no ref.
    /// It collects no bytes — a blob outlives the last asset naming it until a reclamation sweeps it.
    cc::vector<cc::string> removed_assets;
};

/// What a publish actually had to write.
/// Both are zero for a publish that was already durable, which is what idempotence looks like from the outside.
struct vdoc::file::publish_result
{
    isize ops_written = 0;
    isize blobs_written = 0;

    [[nodiscard]] friend bool operator==(publish_result const& a, publish_result const& b) = default;
};

/// What a reclamation collected.
struct vdoc::file::reclaim_result
{
    isize assets_removed = 0;
    isize blobs_removed = 0;

    [[nodiscard]] friend bool operator==(reclaim_result const& a, reclaim_result const& b) = default;
};

/// What one snapshot write actually did — the same result for persisting a snapshot and for pruning.
///
/// `ops_skeletonized` is zero when nothing was pruned, and zero again on a second prune of the same head, which is
/// what its idempotence looks like from the outside.
struct vdoc::file::snapshot_write_result
{
    isize snapshots_written = 0;
    isize ops_skeletonized = 0;

    [[nodiscard]] friend bool operator==(snapshot_write_result const& a, snapshot_write_result const& b) = default;
};

/// What recovering history from a peer actually did.
///
/// All three are zero for a batch this replica already held in full, which is what its idempotence looks like from the
/// outside.
struct vdoc::file::recovery_result
{
    /// Ops this replica did not have at all.
    isize ops_added = 0;

    /// Skeletons left by a prune whose payload the batch put back.
    isize skeletons_filled = 0;

    /// Required snapshots this recovery made unnecessary, and therefore demoted and unpinned.
    isize snapshots_demoted = 0;

    [[nodiscard]] friend bool operator==(recovery_result const& a, recovery_result const& b) = default;
};

/// What a consumer needs to fetch an asset: the record, and the source to fetch its parts through.
///
/// The record is a COPY, so it cannot dangle behind a publish that rewrites the index.
struct vdoc::file::asset_resolution
{
    asset_record record;
    std::shared_ptr<blob_source> blobs;
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
    /// **Enqueue-and-return.** This never blocks and never re-enters its caller, whatever state the store is in — it
    /// may be called with a caller's own lock held, so the answer always arrives later, even where it is already known.
    [[nodiscard]] cc::shared_async<cc::vector<byte>> load(blob_hash const& hash);

    /// A range of the decoded bytes of `hash`; a negative `size` means to the end.
    ///
    /// Same enqueue-and-return contract as load().
    /// This is what chunking was paid for: a consumer wanting a 64-byte header out of a multi-gigabyte part never
    /// materializes the part.
    [[nodiscard]] cc::shared_async<cc::vector<byte>> load_range(blob_hash const& hash, i64 offset, i64 size);

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
    ///
    /// **Null where that load failed hard**, which is this arm's equivalent of a file that will not open.
    /// A required snapshot that will not decode is the one way an image produces one.
    [[nodiscard]] static store_handle create_in_memory(std::shared_ptr<memory_image> image);

    // the loaded document
public:
    /// Loaded and verified in full, eagerly, at open.
    [[nodiscard]] vdoc::op_graph const& ops() const { return _ops; }

    /// The named heads, kept verbatim — including one whose op this load dropped, which is reported as a dangling ref.
    [[nodiscard]] cc::map<cc::string, vdoc::op_id> const& refs() const { return _refs; }

    /// What the file records about each snapshot — never its bytes.
    [[nodiscard]] cc::map<vdoc::op_id, snapshot_entry> const& snapshots() const { return _snapshots; }

    /// The materialization cache this store's snapshots live in.
    ///
    /// Non-const because materializing through it is what a caller wants:
    /// `store.ops().materialize(head, store.snapshot_cache())`.
    /// Required snapshots in here are PINNED, so clear_unpinned() cannot destroy what a prune stood on.
    [[nodiscard]] vdoc::snapshot_cache& snapshot_cache() { return _snapshot_cache; }

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

    /// Keeps `roots` and everything reachable from them through declared dependencies, and deletes the rest.
    ///
    /// The closure is a flood fill over the resident asset index, so cycles are ordinary and a dependency naming
    /// nothing in this file is simply skipped.
    /// Blobs are then marked from the RETAINED assets alone and the rest are swept, chunks following by cascade.
    ///
    /// **An asset remap may legitimately orphan blobs** — that is the case this exists for, not a bug to prevent.
    /// Reclaiming with no roots at all empties the asset index, which is a legitimate ask and not guarded against.
    [[nodiscard]] cc::shared_async<reclaim_result> reclaim(cc::span<cc::string const> roots);

    // snapshots and pruning
public:
    /// Persists the cached snapshots at `ops` as DROPPABLE rows.
    ///
    /// **Explicit, and never a side effect of publish.** A snapshot is derived, so writing one on a heuristic would
    /// make publishing non-idempotent and grow the file with caches nobody asked for.
    /// An op with no cached snapshot is skipped rather than reported: the cache is derived and may have evicted it.
    [[nodiscard]] cc::shared_async<snapshot_write_result> publish_snapshots(cc::span<vdoc::op_id const> ops);

    /// Attaches a REQUIRED snapshot at `head` and empties every op behind it that no other ref still needs.
    ///
    /// **Destructive, explicit, and never automatic.** Less storage and a faster load, against losing deep history and
    /// shortening the range over which two replicas can still synchronize.
    ///
    /// What remains where an op was is a SKELETON — its id and its parents, with no payload.
    /// The DAG keeps its shape, so reachability, merges and child walks all still work; only the content is gone.
    /// Deleting the rows instead would sever ancestry, and two writes that were ordered would read as concurrent.
    ///
    /// **Every ref must descend from `head`**, and this fails without writing anything otherwise.
    ///
    /// A required snapshot carries no `superseded`, which is sound only while nothing can present a writer from behind
    /// it.
    /// A ref that forked earlier can — its branch keeps its own ancestors — and merging the two would then fabricate
    /// a multi-value nobody authored, while replaying instead would read the pruned ops as empty.
    /// So the boundary a document may prune to is the oldest op every ref still descends from.
    ///
    /// Also fails, writing nothing, where `head` is not in the graph.
    [[nodiscard]] cc::shared_async<snapshot_write_result> prune(vdoc::op_id const& head);

    // recovering history from a peer
public:
    /// Takes history from a peer nobody trusts, verifying every op against its own bytes before storing any.
    ///
    /// **No trust in the sender at any point.** An op id recursively commits to everything behind it, so recomputing
    /// the hashes is the whole check — see [recovery](../../../versioned-document/src/versioned-document/recovery.hh).
    /// The batch is a SET: a partial or hostile one is refused naming the op, and leaves this replica exactly as it
    /// was, in memory and in storage alike.
    /// A skeleton this replica holds is FILLED IN, which add_op deliberately will not do.
    ///
    /// **Refuses a batch that would leave a live writer behind a still-required snapshot.**
    /// A required snapshot carries no `superseded`, so an op forking below it can present a writer nothing suppresses
    /// — the very failure prune refuses to create, arriving from the other direction.
    /// Sending the rest of that snapshot's ancestry in the same batch is what makes such a batch acceptable, and doing
    /// so DEMOTES the snapshot to droppable; that is why the refusal is a boundary rather than a ban.
    [[nodiscard]] cc::shared_async<recovery_result> recover(cc::span<vdoc::received_op const> batch);

    // resolving assets
public:
    /// An asset id -> its metadata, its ordered parts, and a source to fetch them through.
    ///
    /// **This is where this library stops.** Caching, eviction, streaming, format dispatch and decode-to-GPU are all
    /// downstream, because a file is one source of assets among many.
    ///
    /// **The single entry point for reaching parts**, and deliberately the only one.
    /// Addressing a part goes through the record it hands back — `main_part()`, `try_find_part`, `parts_named` — so a
    /// caller always works from one snapshot.
    /// A per-part accessor on the store would read the live index each call, and a remap between two of them could
    /// hand back parts from two different assets with nothing able to detect it.
    [[nodiscard]] cc::optional<asset_resolution> resolve_asset(cc::string_view asset_id);

    /// Runs storage work this build must run on the calling thread; true if there may be more.
    ///
    /// **A blob fetch completes here on an in-memory store**, which has no thread of its own — and on any store in a
    /// build without threads.
    /// A file-backed store in a threaded build has nothing to run and returns false, so a caller that pumps its loop
    /// unconditionally is correct everywhere and a caller that never pumps waits forever on one arm.
    bool pump() { return on_pump(); }

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

    // the seam — seven hooks, and nothing else differs
protected:
    store() = default;

    /// Persists one already-computed publish.
    [[nodiscard]] virtual cc::shared_async<publish_result> on_publish(impl::publish_job job) = 0;

    /// Persists one already-computed prune.
    ///
    /// Its own hook rather than a mode of on_publish, because a publish only ever APPENDS and is idempotent by content
    /// addressing, while a prune destroys.
    /// The safest operation in the format should not share a code path with the only destructive one.
    [[nodiscard]] virtual cc::shared_async<snapshot_write_result> on_write_snapshots(impl::snapshot_write_job job) = 0;

    /// Persists one already-computed recovery.
    ///
    /// Its own hook for the same reason pruning has one: the hooks split by what a write can DESTROY.
    /// Publishing appends, pruning destroys, and a recovery fills a hole back in — three kinds, three hooks.
    /// Riding on_publish is not open to it either, since a recovered op has no ref yet and would have to bypass the
    /// reachability check that path exists for.
    [[nodiscard]] virtual cc::shared_async<recovery_result> on_recover(impl::recovery_job job) = 0;

    /// Starts one blob fetch and returns at once.
    ///
    /// **MUST NOT run the fetch inline.** blob_source::load may be called with a caller's lock held, so an
    /// implementation that resolved the promise here would re-enter that caller through the woken continuation —
    /// which is why the arm that could answer instantly is the one that has to queue.
    [[nodiscard]] virtual cc::shared_async<cc::vector<byte>> on_fetch_blob(blob_hash const& hash,
                                                                           impl::blob_fetch_range range) = 0;

    /// Persists one already-computed reclamation.
    [[nodiscard]] virtual cc::shared_async<reclaim_result> on_reclaim(impl::reclaim_job job) = 0;

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

    /// The decoded snapshots, with the REQUIRED ones pinned.
    /// _snapshots above is what the file records; this is what materializing actually uses.
    vdoc::snapshot_cache _snapshot_cache;
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
    friend class blob_source;

    /// Routes one fetch to the implementation, and PUMPS NOTHING.
    ///
    /// That omission is the whole non-re-entrancy contract: pumping here would run the fetch — and the caller's
    /// continuation — inside the caller's own load() call, under whatever lock it holds.
    [[nodiscard]] cc::shared_async<cc::vector<byte>> impl_fetch_blob(blob_hash const& hash, impl::blob_fetch_range range);

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
    /// The one source resolve_asset hands out, remade once every resolution has let go of it.
    std::weak_ptr<blob_source> _shared_source;
    bool _is_closed = false;
};
