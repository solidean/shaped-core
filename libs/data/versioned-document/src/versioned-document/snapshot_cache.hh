#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <versioned-document/op.hh>
#include <versioned-document/snapshot_document.hh>

/// The materialization cache: snapshots keyed by the op they were computed at.
///
/// The design is [snapshots](../../docs/concepts/snapshots.md) and
/// [decisions.md](../../docs/decisions.md#a-snapshot-stores-surviving-only-and-its-validity-is-decided-at-use).

/// Materialization results cached against the op they were computed at.
///
/// **Caller-owned and explicit.** materialize takes one by reference, so nothing here is hidden mutable state reached
/// through a const op_graph, and nothing installs behind the caller's back.
///
/// An entry is `surviving` and nothing else — exactly a raw_document — which is a well-defined function of its op's
/// own causal past and independent of the sweep that computed it.
/// So an entry is always sound to STORE, and there is no eligibility question at install time.
/// **Whether it may be USED is decided per sweep, against today's DAG** — see impl::materialize.
///
/// **Adding ops never invalidates an entry, and there is deliberately no hook that says otherwise.**
/// An op id is a content hash committing to everything behind it, so surviving(id) cannot change once computed.
class vdoc::snapshot_cache
{
public:
    /// How much the cache is allowed to keep.
    struct budget
    {
        /// Unpinned entries kept; the least recently used goes when a new one arrives.
        /// A PINNED entry is never evicted and never counted here.
        isize max_unpinned_entries = 8;
    };

    snapshot_cache() = default;
    explicit snapshot_cache(budget b) : _budget(b) {}

    /// The snapshot at `id`, or null.
    /// Not const: a hit is what the eviction order is kept by.
    [[nodiscard]] snapshot_document const* find(op_id const& id);

    /// Membership without touching the eviction order — what the sweep's terminator test asks, once per op.
    [[nodiscard]] bool contains(op_id const& id) const { return _entries.contains(id); }

    /// Installs the snapshot at `id`, replacing whatever was there.
    ///
    /// `doc` must be the UNFILTERED materialization of `id` as a SINGLE head.
    /// A filtered result is a projection rather than surviving(id), and installing one silently truncates every later
    /// sweep that terminates here — the one way this cache can be poisoned.
    ///
    /// **A composed document is a projection too, and a worse one.** What a
    /// [layer_stack](layer_stack.hh) composes is several histories at once, and a
    /// [direct layer](direct_layer.hh)'s values are attributed to a synthetic writer id that names no op at all — so
    /// installing one would put a fabricated op id into a file.
    ///
    /// `pinned` is what a persisted, load-bearing snapshot is: never evicted, and never counted against the budget.
    void install(op_id const& id, snapshot_document doc, bool pinned = false);

    /// Drops `id`, pinned or not — the in-memory half of deleting a persisted snapshot.
    bool erase(op_id const& id);

    /// Hands the entry at `id` out and removes it, so a caller can derive the next snapshot from it instead of
    /// recomputing one.
    ///
    /// `advance_snapshot` is what this exists for, and taking a snapshot in order to install it somewhere else is the
    /// only sound use: what comes back is `surviving(id)`, and installing it under any other op is a lie the cache
    /// cannot detect.
    [[nodiscard]] cc::optional<snapshot_document> take(op_id const& id);

    /// Drops every unpinned entry.
    /// **Must be invisible**: this may change how long a materialization takes, and nothing else about it.
    /// Turns a pinned entry back into an ordinary one, keeping its bytes.
    ///
    /// What a snapshot stops being load-bearing looks like from here: the materialization is still correct, so nothing
    /// is recomputed — it simply becomes evictable again.
    /// False where the cache does not have the entry; unpinning one that is not pinned is a no-op and true.
    bool unpin(op_id const& id);

    void clear_unpinned();

    /// Drops everything, pins included.
    void clear();

    [[nodiscard]] isize size() const { return _entries.size(); }
    [[nodiscard]] bool is_pinned(op_id const& id) const;
    [[nodiscard]] isize pinned_count() const;

    /// The value bytes every entry owns together, which is what a memory budget would look at.
    [[nodiscard]] isize owned_byte_size() const;

    [[nodiscard]] budget const& get_budget() const { return _budget; }

private:
    struct entry
    {
        snapshot_document doc;
        bool pinned = false;
        u64 last_used = 0;
    };

    /// Evicts least-recently-used unpinned entries until the budget holds.
    /// A linear scan, because max_unpinned_entries is single-digit and a scan keeps the order trivially reproducible.
    void impl_trim();

    cc::map<op_id, entry> _entries;

    /// A monotone counter rather than a clock, so eviction order is reproducible in a test.
    u64 _tick = 0;

    budget _budget;
};

/// When a snapshot is worth its memory.
///
/// Passed in rather than stored on the cache, so the cost is visible at the call site and never a hidden property of
/// the thing being written to.
struct vdoc::snapshot_policy
{
    /// Install only where the walk from the head to the nearest cached snapshot crosses at least this many ops.
    isize min_ops_behind = 4096;
};

namespace vdoc
{

/// Materializes `head` unfiltered and installs the snapshot at it.
///
/// For when the application knows the good place — immediately after a load, say.
/// Idempotent, and false only where the graph does not have `head`.
bool install_snapshot(op_graph const& graph, op_id const& head, snapshot_cache& cache);

/// Installs a snapshot at `head` only where the walk behind it is long enough to pay for one.
///
/// The probe stops counting at `min_ops_behind`, so asking costs at most that many ops even on a long history.
/// Returns true where a snapshot was installed.
bool install_snapshot_if_useful(op_graph const& graph,
                                op_id const& head,
                                snapshot_cache& cache,
                                snapshot_policy policy = {});

/// Moves the snapshot at `parent` forward onto its single-parent child, in place.
///
/// **`surviving(child) = surviving(parent)` with the child's assignments overwriting their paths, and that identity
/// holds exactly on a single-parent edge**: the child dominates every writer it overwrites and contributes no other.
/// So this costs the child's assignments rather than a materialization, and a session that calls it per accepted op
/// keeps the head permanently one op from a snapshot.
///
/// The entry is **re-keyed**: it is removed from `parent` and installed at `child`, pin and all.
/// Nothing about "an op id commits to everything behind it" is bent — the content really is surviving(child) — but
/// every `raw_document` borrowing the old entry is invalidated, exactly as raw_document.hh says a cache modification
/// does.
///
/// **Caller-driven on purpose.** During a wide fan the frames are siblings, so advancing onto one would leave every
/// other frame with no snapshot to terminate at and a whole history to replay.
/// The snapshot stays at the branch point until a frame is accepted as history, and only the application knows when
/// that is.
///
/// False, changing nothing, where the graph does not have `child`, where `child` is a skeleton or does not have
/// exactly the one parent `parent`, or where the cache has no entry at `parent`.
bool advance_snapshot(op_graph const& graph, snapshot_cache& cache, op_id const& parent, op_id const& child);
} // namespace vdoc
