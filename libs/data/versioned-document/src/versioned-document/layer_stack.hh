#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <versioned-document/change_summary.hh>
#include <versioned-document/direct_layer.hh>
#include <versioned-document/document_builder.hh>
#include <versioned-document/impl/compose.hh>
#include <versioned-document/incremental_parse.hh>
#include <versioned-document/parse.hh>

/// Composing several independent histories into one document.
///
/// The design is [layering](../../docs/concepts/layering.md).

/// Names one layer in a stack, and keeps naming it as others come and go.
///
/// **Never an index.** A push shifts nothing today and a reorder would shift everything, and a handle that silently
/// re-targeted would be the worst kind of bug here — it would compose a document nobody asked for and look fine.
struct vdoc::layer_handle
{
    u32 index = 0;

    /// 0 is the invalid handle, which is what a failed lookup reads as.
    u32 generation = 0;

    [[nodiscard]] bool is_valid() const { return generation != 0; }

    [[nodiscard]] friend bool operator==(layer_handle const&, layer_handle const&) = default;
};

/// Knobs on a layered apply.
struct vdoc::layered_apply_options
{
    /// How far back a graph layer's chain walk may go before the whole compose is redone.
    isize max_chain_ops = 64;

    /// Compact when the composed document's dead arena bytes exceed this multiple of its live bytes; 0 never compacts.
    f64 compaction_ratio = 1.0;

    /// Recompose everything unconditionally, which is what the differential test pins the fast path against.
    bool force_rebuild = false;
};

/// What one layered apply did.
struct vdoc::layered_apply_stats
{
    bool took_fast_path = false;

    /// `none` exactly when `took_fast_path`.
    apply_fallback_reason fallback_reason = apply_fallback_reason::none;

    /// The layer whose delta could not be derived, where that is what forced the rebuild.
    layer_handle stale_layer;

    isize composed_entities = 0;
    bool compacted = false;
};

/// An ordered stack of layers, composed into one ordinary `document`.
///
/// Each layer has its own history, its own hashes and its own snapshots, and **a higher layer replaces a lower one per
/// property path** — so this is the totally ordered, conflict-free composition, in deliberate contrast to a DAG merge.
/// A conflict can only ever be layer-local.
///
///     auto stack = vdoc::layer_stack();
///     auto const base = stack.push_direct_layer("base", produced_every_frame);
///     auto const user = stack.push_graph_layer("user", graph, head);
///     stack.rebuild(policy, report, changes);
///     use(stack.composed());
///
/// **The composed document is an ordinary `document`.** Nothing downstream of `composed()` learns that layering exists:
/// the dense columns, `each<A, B>`, immutability and the safe-to-hand-to-another-thread guarantee are all unchanged.
///
/// **The stack pulls every delta rather than being told one.**
/// It holds each graph layer's head — `set_head` is the only way to move one — and each direct layer bumps its own
/// version, so "the caller forgot to mention a layer moved" is not expressible rather than merely detected.
/// What it cannot always derive is *how* a layer moved; that falls back to a full recompose and says so in the stats.
///
/// **Lifetimes are the caller's**, and for the whole life of the stack: every `op_graph`, `snapshot_cache` and
/// `direct_layer` handed in must outlive it, and a direct layer belongs to exactly one stack.
class vdoc::layer_stack
{
    // lifetime
public:
    layer_stack();
    ~layer_stack();

    layer_stack(layer_stack&&) noexcept;
    layer_stack& operator=(layer_stack&&) noexcept;
    layer_stack(layer_stack const&) = delete;
    layer_stack& operator=(layer_stack const&) = delete;

    // structure
public:
    /// Appends a layer materialized from an op graph, on top of everything already here.
    /// `cache` is where its materializations may terminate; null is correct and slow.
    layer_handle push_graph_layer(cc::string_view name, op_graph const& graph, op_id head, snapshot_cache* cache = nullptr);

    /// Appends a directly written layer on top of everything already here.
    layer_handle push_direct_layer(cc::string_view name, direct_layer& layer);

    /// Moves a graph layer's head, which is the only way one moves.
    void set_head(layer_handle handle, op_id head);

    [[nodiscard]] op_id head_of(layer_handle handle) const;

    /// Takes a layer out of the composition without removing it.
    ///
    /// **Not a fast path.** Computing what muting changed means enumerating the layer's paths, which for a base layer is
    /// the whole document — so this forces a full recompose, reported as `structure_changed`.
    /// The cheap way to withdraw a layer's contribution incrementally is to abstain its paths.
    void set_muted(layer_handle handle, bool muted);

    [[nodiscard]] bool is_muted(layer_handle handle) const;

    [[nodiscard]] isize layer_count() const;

    /// Bottom-first, which is the order layers were pushed in.
    [[nodiscard]] layer_handle layer_at(isize index) const;

    // the composed document
public:
    [[nodiscard]] document const& composed() const { return _composed; }

    /// Recomposes from nothing, which is always correct and O(document).
    void rebuild(parse_policy const& policy, parse_report& report, change_summary& out_changes);

    /// Brings the composed document up to date with whatever the layers now say.
    ///
    /// Re-interprets only the entities some layer reports dirty, and falls back to `rebuild` where it cannot derive a
    /// layer's delta — correct either way, and `stats` says which ran and why.
    void apply(parse_policy const& policy,
               parse_report& report,
               change_summary& out_changes,
               layered_apply_options options = {},
               layered_apply_stats* stats = nullptr);

    // provenance
public:
    /// Which layer this path's value comes from, or an invalid handle where no layer has it.
    ///
    /// **Per path, and it cannot be per component**: replacement is per path, so one typed component may be assembled
    /// from several layers.
    ///
    /// This materializes the entity per layer on demand rather than keeping every layer's document resident, so it is a
    /// UI query and not something to run in a loop.
    [[nodiscard]] layer_handle provenance_of(property_path const& path) const;

private:
    /// One slot in the stack: what it is, where it currently is, and where the composed document thinks it is.
    struct layer
    {
        cc::string name;
        u32 generation = 0;
        bool muted = false;

        /// Set for a graph-backed layer, and then `direct` is null.
        op_graph const* graph = nullptr;
        op_id head;
        snapshot_cache* cache = nullptr;

        /// Set for a directly written layer, and then `graph` is null.
        direct_layer* direct = nullptr;

        /// Where the composed document was last told this layer is.
        /// An op id for a graph layer, a counter for a direct one — one is a content address and the other is not, which
        /// is why they are separate fields rather than a shared "version".
        op_id recorded_head;
        u64 recorded_version = 0;

        [[nodiscard]] bool is_graph() const { return graph != nullptr; }
    };

    [[nodiscard]] layer* impl_find(layer_handle handle);
    [[nodiscard]] layer const* impl_find(layer_handle handle) const;

    /// Every unmuted layer's raw document, bottom-first, over `_materialized`.
    [[nodiscard]] cc::vector<impl::layer_view> impl_views() const;

    /// Materializes each unmuted graph layer, restricted to `entities` when that is non-empty.
    void impl_materialize(cc::span<entity_id const> entities);

    /// Records every layer's current version as the one the composed document is at.
    void impl_record_versions();

    /// Drains every direct layer's pending changes, so a rebuild does not leave them to replay.
    void impl_discard_pending_changes();

    cc::vector<layer> _layers;

    /// Per layer, parallel to `_layers` — a graph layer's materialization, empty for a direct or muted one.
    cc::vector<raw_document> _materialized;

    document _composed;

    u32 _next_generation = 1;

    /// Set until the first compose, and by any change to the set of layers or their muting.
    bool _structure_dirty = true;
};
