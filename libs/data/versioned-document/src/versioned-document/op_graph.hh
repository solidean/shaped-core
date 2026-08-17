#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <versioned-document/op.hh>
#include <versioned-document/raw_document.hh>

/// The op DAG: ops keyed by content hash, plus materialization of one or several heads.
///
/// The graph holds ops and answers questions about their shape.
/// **Heads are storage's concern, not the graph's** — nothing here tracks a current head or moves one.
///
/// A graph tolerates ops it does not have.
/// A child can arrive before its parent, and pruning leaves parents that are gone entirely, so reachability skips
/// what is missing rather than failing.
///
/// The design is [the concept](../../docs/concepts/ops-and-content-addressing.md#the-dag).
class vdoc::op_graph
{
public:
    /// Inserts an op, keyed by its content hash, and returns that id.
    ///
    /// **Idempotent, and not an append.** Re-adding a hash already present changes nothing: the stored op stays, the
    /// child index is not disturbed, and no head moves.
    op_id add(op o);

    /// The op with this id, or null if the graph does not have it.
    [[nodiscard]] op const* find(op_id const& id) const;

    [[nodiscard]] bool contains(op_id const& id) const { return find(id) != nullptr; }
    [[nodiscard]] isize size() const { return _ops.size(); }

    /// The ops naming this one as a parent, in the order they were added.
    ///
    /// These are the inverted parent edges, which a parent-only op cannot answer and a downstream walk needs.
    /// Keys may name ops the graph does not have, since a child can arrive first; the values are always ops that are.
    ///
    /// **The order is arrival order, not content order** — a caller that needs a reproducible one sorts by id bytes.
    [[nodiscard]] cc::span<op_id const> children(op_id const& id) const;

    /// Every op reachable from `heads` through parent edges, including the heads, sorted by id bytes.
    ///
    /// Missing ops are skipped rather than reported: this is the local closure, and a pruned or not-yet-received
    /// parent is a normal state rather than an error.
    [[nodiscard]] cc::vector<op_id> collect_reachable(cc::span<op_id const> heads) const;

    /// Every op reachable from `heads`, stopping at any op `is_terminator` accepts, sorted by id bytes.
    ///
    /// A terminator is INCLUDED and its parents are not expanded, so the result is parent-closed except at
    /// terminators.
    /// That is what makes the in-degree-0 set of the result the frontier a snapshot-seeded sweep validates against.
    [[nodiscard]] cc::vector<op_id> collect_reachable_until(cc::span<op_id const> heads,
                                                            cc::function_ref<bool(op_id const&)> is_terminator) const;

    /// Replaces an op's payload with nothing, leaving its id and its parent edges exactly where they are.
    ///
    /// Removing the entry instead would sever ancestry through it, and two writes that were ordered would read as
    /// concurrent — a semantic change caused by a storage operation.
    ///
    /// **Nothing here checks that a snapshot covers what this erases**; that is the pruning caller's job.
    /// False where the graph does not have the op; an op that is already a skeleton is a no-op and true.
    bool skeletonize(op_id const& id);

    /// Puts a payload back on a skeleton, leaving its id and its parent edges exactly where they are.
    ///
    /// The exact inverse of skeletonize, and what receiving history from a peer needs: `add` is idempotent by id, so
    /// it leaves a skeleton a skeleton.
    ///
    /// **The payload must already have been verified against `id`**, and nothing here re-checks it — the route that
    /// does is [recovery](recovery.hh), which is what a receiver calls.
    /// An op that already has its payload is left alone, since content addressing makes the two byte-identical.
    /// False where the graph does not have the op.
    bool fill_payload(op_id const& id, op_payload payload);

    /// Forgets an op entirely — id, payload and parent edges — where nothing descends from it.
    ///
    /// This is the opposite situation from skeletonize, which the similar shape hides.
    /// A skeleton exists because ancestry must survive a pruned op; this is for an op that has **no ancestry to
    /// preserve** because nothing came after it, and that nothing outside has seen.
    /// The case is a [discarded editing frame](../../docs/concepts/workloads.md#continuous-editing-goes-wide): every
    /// frame of a drag is a new op off the same state, and all but the last are thrown away.
    ///
    /// Content addressing is what makes forgetting safe rather than lossy: the op is a pure function of its content,
    /// so if it ever comes back — from an undo, from a peer — `add` recreates it byte-identically.
    ///
    /// **Asserts if the op has children in this graph.** Severing ancestry is skeletonize's job and never this one.
    /// False where the graph does not have the op.
    bool drop_leaf(op_id const& id);

    /// Every op in the graph that nothing descends from, sorted by id bytes.
    /// What a session sweeps to find the frames it abandoned, so it need not have tracked them.
    [[nodiscard]] cc::vector<op_id> leaves() const;

    /// Materializes the document as of one head, or as of several merged.
    ///
    /// Materializing several heads is defined to equal materializing a merge op over them, for every property
    /// neither side contests.
    ///
    /// The result borrows this graph's op bytes — see [raw_document](raw_document.hh).
    [[nodiscard]] raw_document materialize(op_id const& head) const;
    [[nodiscard]] raw_document materialize(cc::span<op_id const> heads) const;

    /// The same pass, restricted to the named entities.
    ///
    /// The filter applies to *assignments*, never to edges: filtering edges would sever ancestry and fabricate
    /// multi-values.
    /// This is what op_builder diffs against, so it must agree with the unfiltered pass exactly.
    [[nodiscard]] raw_document materialize_entities(cc::span<op_id const> heads, cc::span<entity_id const> entities) const;

    /// The same three passes, terminating the walk at cached snapshots wherever that is sound today.
    ///
    /// The result is defined to equal the uncached overload above, always.
    /// Where the cache cannot be used the sweep silently replays instead, so this differs only in how long it takes.
    ///
    /// **The result borrows the cache as well as the graph** — see [raw_document](raw_document.hh).
    /// Nothing is installed here: installing is [snapshot_cache](snapshot_cache.hh)'s explicit business.
    [[nodiscard]] raw_document materialize(op_id const& head, snapshot_cache& cache) const;
    [[nodiscard]] raw_document materialize(cc::span<op_id const> heads, snapshot_cache& cache) const;
    [[nodiscard]] raw_document materialize_entities(cc::span<op_id const> heads,
                                                    cc::span<entity_id const> entities,
                                                    snapshot_cache& cache) const;

private:
    cc::map<op_id, op> _ops;
    cc::map<op_id, cc::vector<op_id>> _children;
};

namespace vdoc::impl
{
/// What one materialization pass did, for a test that has to see the cache was actually used.
///
/// Comparing a cached result against an uncached one proves nothing if the cache was never consulted, and that is the
/// most likely way this whole area passes vacuously.
struct materialize_stats
{
    /// Ops the sweep actually processed, which is the number a snapshot is there to shrink.
    isize ops_walked = 0;

    /// 1 where the sweep was seeded from a snapshot, 0 where it replayed.
    isize snapshots_used = 0;

    /// Entities read out of the seeding snapshot.
    ///
    /// A FILTERED sweep must read only the entities it was asked for, or seeding a one-entity diff costs a walk of the
    /// whole document and the snapshot buys nothing at exactly the size it was meant for.
    /// That is invisible in a result comparison, so it is reported rather than trusted.
    isize snapshot_entities_read = 0;

    /// True where a snapshot was found and the validity gate then rejected it, so the sweep re-ran without one.
    bool fell_back = false;
};

/// Knobs on the materialization pass, and the seam the snapshot cache attaches to.
struct materialize_options
{
    /// Drop the superseded sets wholesale where exactly one live state remains in the sweep.
    ///
    /// Without it a linear history of N writes to one path accumulates N-1 superseded entries, which makes the
    /// reference pass quadratic — so this is on in production and off only to prove it changes no result.
    bool drop_superseded_at_articulation_points = true;

    /// Terminate the walk at cached snapshots and seed from one where the gate allows it.
    /// Null is the plain replay, which is what a cached sweep is checked against.
    snapshot_cache* cache = nullptr;

    /// Where the sweep records what it did, or null.
    materialize_stats* stats = nullptr;
};

/// The one materialization pass, which every public entry point above forwards to.
/// An empty `entities` means no filter.
[[nodiscard]] raw_document materialize(op_graph const& graph,
                                       cc::span<op_id const> heads,
                                       cc::span<entity_id const> entities,
                                       materialize_options options);
} // namespace vdoc::impl
