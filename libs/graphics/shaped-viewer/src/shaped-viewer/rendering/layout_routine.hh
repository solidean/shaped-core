#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/render_plan.hh>

/// Everything a layout pipeline bakes in.
///
/// The sampler and the source sub-rects are deliberately absent: the sampler is a dynamic one in the transient group
/// and the rects ride in inline constants, so one pipeline serves every rect and every filter.
struct sv::impl::layout_pipeline_key
{
    sg::pixel_format format = sg::pixel_format::undefined;
    draw_kind kind = draw_kind::view;
    bool blended = false;

    [[nodiscard]] bool operator==(layout_pipeline_key const&) const = default;

    [[nodiscard]] friend u64 hash(layout_pipeline_key const& k)
    {
        return cc::make_hash(u64(k.format), u64(k.kind), u64(k.blended));
    }
};

/// The textures a plan's targets and traces resolved to, index-parallel to `render_plan::targets` / `::traces`.
///
/// Handed to the routine rather than looked up by it, because allocating and reclaiming them is the view renderer's
/// job and doing it here would mean taking that routine's lock inside an open rendering scope.
struct sv::plan_textures
{
    cc::span<sg::texture_2d const> targets;
    cc::span<sg::texture_2d const> traces;
};

/// Draws one target's whole share of a frame: the layout's border bands, then each view where the layout put it.
///
/// It is the only thing that writes a view target or the frame's output, so compositing order, blending, fitting and
/// sampling live in exactly one place.
/// One call fills one open rendering scope — the plan already grouped its draws by target, and the caller opens the pass.
///
/// Window-aware: pipelines are keyed by the target's format together with the draw kind, and a frame drives one call
/// per window's output.
///
/// Nothing here allocates or reclaims a texture, and nothing takes another routine's lock, so a caller may drive it
/// from inside a scope they own.
class sv::layout_routine : public sg::render_routine<layout_routine>
{
public:
    /// Records `draws` onto the open `scope`, reading each source out of `textures`.
    ///
    /// The scope's first color target is what the pipelines are built for, and each draw sets its own viewport and
    /// scissor from its rect.
    /// A draw whose source is missing is skipped rather than drawn black.
    /// A no-op if the shaders did not compile — an exception here would unwind out of the caller's open scope and
    /// leave their command list unsubmitted.
    static void execute(sg::rendering_scope& scope,
                        window_id window,
                        cc::span<layout_draw const> draws,
                        plan_textures const& textures);

protected:
    void init_declare(sg::context& ctx) override;

private:
    sg::binding_group_layout_handle _group_layout;

    /// Mutable because the cache guards itself and its whole acquire path is const, so a lazy build needs no routine lock.
    mutable sr::keyed_pipeline_cache<impl::layout_pipeline_key> _pipelines;
};
