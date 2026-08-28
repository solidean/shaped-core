#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/keyed_cache.hh>
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/view/view_data.hh> // temporal_id::accumulation, for the default argument
#include <shaped-viewer/view/view_id.hh>

/// Everything one viewer's views keep across frames, keyed by view_id.
///
/// A view is re-submitted every frame as a fresh `view_data`; its id is what ties it to the record here.
/// Both halves of that record live together — the camera and placement a caller drives, and the composite target and
/// accumulators the trace writes — so one reclamation decides both.
/// That is the invariant the type exists for: an identity cannot outlive its texture, and a texture cannot be released
/// out from under the parent still sampling it.
///
/// Owned by whoever runs the frame, and handed to `view_renderer` / `viewer_renderer` rather than reached out of them.
/// A viewer owns one; a caller driving `view_renderer` directly owns one of their own.
/// One store per viewer, never per context: two viewers name their views in separate id spaces.
///
/// Not thread-safe, and deliberately so — the frame that owns it is single-threaded, and the routines it is handed to
/// take their own guards around the recording, not around this.
class sv::view_store
{
public:
    view_store();

    /// Reclaim what has gone idle or over budget, then advance to `epoch`.
    /// The frame's job, once, before anything resolves or traces — its own reclamation runs against the just-finished
    /// frame's usage, so the frame about to run keeps its working set.
    void begin_frame(u64 epoch);

    /// The record for `id`, default-constructed on first use, marked used this frame.
    [[nodiscard]] impl::view_state& get_or_create(view_id id);

    /// The record for `id`, or null if absent; a hit is marked used this frame.
    [[nodiscard]] impl::view_state* get_ptr(view_id id);

    /// The record for `id`, which must exist; marked used this frame.
    [[nodiscard]] impl::view_state& get(view_id id);

    /// The record for `id` without marking it used, or null if absent — for hit-tests and queries, which must not keep
    /// a view alive.
    [[nodiscard]] impl::view_state const* peek_ptr(view_id id) const;

    /// The record for `id` without marking it used; `id` must exist.
    [[nodiscard]] impl::view_state const& peek(view_id id) const;

    /// What this view's textures cost against the store's budget.
    /// `id` must exist.
    void set_payload_bytes(view_id id, isize bytes);

    /// How many frames the temporal resource `temporal_id` of view `id` has accumulated.
    /// 0 for a resource that restarted this frame, or that was never seen.
    ///
    /// Defaults to the first traced layer's accumulator, which is the one a single-layer view has.
    /// For tests and debug overlays — the trace needs none of it.
    [[nodiscard]] u32 accumulated_frames(view_id id, u64 temporal_id = temporal_id::accumulation(0)) const;

    /// The lowest accumulated-frame count across every traced layer of `id`, and 0 for a view with none.
    /// See `impl::min_accumulated_frames`, which is the rule.
    [[nodiscard]] u32 min_accumulated_frames(view_id id) const;

    /// Whether every traced layer of `id` has reached `frames`, or has stopped climbing and never will.
    /// See `impl::is_accumulation_converged`, which is the rule; false for a view that is not here.
    [[nodiscard]] bool is_accumulation_converged(view_id id, cc::optional<u32> frames = {}) const;

    [[nodiscard]] isize count() const { return _entries.count(); }
    [[nodiscard]] isize payload_bytes() const { return _entries.payload_bytes(); }

private:
    impl::keyed_cache<view_id, impl::view_state> _entries;
};
