#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
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
/// The design is [the concept](../../docs/concept.md#the-dag).
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

    /// The ops naming this one as a parent, sorted by id bytes.
    ///
    /// These are the inverted parent edges, which a parent-only op cannot answer and a downstream walk needs.
    /// Keys may name ops the graph does not have, since a child can arrive first.
    [[nodiscard]] cc::span<op_id const> children(op_id const& id) const;

    /// Every op reachable from `heads` through parent edges, including the heads, sorted by id bytes.
    ///
    /// Missing ops are skipped rather than reported: this is the local closure, and a pruned or not-yet-received
    /// parent is a normal state rather than an error.
    [[nodiscard]] cc::vector<op_id> collect_reachable(cc::span<op_id const> heads) const;

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

private:
    cc::map<op_id, op> _ops;
    cc::map<op_id, cc::vector<op_id>> _children;
};

namespace vdoc::impl
{
/// Knobs on the materialization pass that exist only so a test can compare the two modes against each other.
struct materialize_options
{
    /// Drop the superseded sets wholesale where exactly one live state remains in the sweep.
    ///
    /// Without it a linear history of N writes to one path accumulates N-1 superseded entries, which makes the
    /// reference pass quadratic — so this is on in production and off only to prove it changes no result.
    bool drop_superseded_at_articulation_points = true;
};

/// The one materialization pass, which every public entry point above forwards to.
/// An empty `entities` means no filter.
[[nodiscard]] raw_document materialize(op_graph const& graph,
                                       cc::span<op_id const> heads,
                                       cc::span<entity_id const> entities,
                                       materialize_options options);
} // namespace vdoc::impl
