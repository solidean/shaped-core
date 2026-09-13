#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
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
class sv::layout_routine : public sg::render_routine<layout_routine, sg::pixel_format>
{
public:
    /// Records `draws` onto the open `scope`, reading each source out of `textures`.
    ///
    /// The scope's first color target is what the pipelines are built for, and each draw sets its own viewport and
    /// scissor from its rect.
    /// A draw whose source is missing is skipped rather than drawn black.
    /// Declines, recording nothing, while the shaders are still building and after a build that failed — never by
    /// throwing, which would unwind out of the caller's open scope and leave their command list unsubmitted.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     window_id window,
                                                     cc::span<layout_draw const> draws,
                                                     plan_textures const& textures);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// How many pipelines one instance holds: every draw_kind, blended and not.
    static constexpr int k_draw_kinds = 4;

    sg::binding_group_layout_handle _group_layout;

    /// Every pipeline this format needs, indexed [kind][blended], built during init.
    ///
    /// The format does not vary within one invocation, so it parametrizes the routine — but kind and blend DO vary
    /// per draw, which is why they are declared here rather than making the routine one instance per draw.
    /// Six of the eight slots are reachable: a flat fill is always blended, so background and border have no unblended
    /// form and those two stay null.
    ///
    /// Built up front rather than on demand precisely because the alternative is a build on the frame path, inside the
    /// caller's open rendering scope — which is where nothing may wait.
    sg::raster_pipeline_handle _pipelines[k_draw_kinds][2];
};
