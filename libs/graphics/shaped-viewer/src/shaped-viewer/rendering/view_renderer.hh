#pragma once

#include <clean-core/container/map.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/view_id.hh>

namespace sg
{
struct color_target;
}

namespace sv
{
/// The per-frame view renderer — itself a render routine, and the orchestrator that composes the leaf routines.
///
/// It resolves each view's scene items to GPU resources through the managers, path-traces the view into a transient target (driving sv::pathtrace_routine), then binds the caller's output target, opens the raster scope, and blits the view across it (driving sr::blit_routine).
/// It never records a trace / draw itself — that rule stays in the leaf routines.
/// This one only sequences them and owns the per-view persistent cache.
///
/// Being a routine, that persistent cache — keyed by view_id, the temporal-accumulation seam — lives on the per-context instance under the routine's own lock, rather than in a value the caller has to keep around.
///
/// It is the only one of the three that takes a guard: both leaves it drives read-only, so driving them under its own guard nests no lock.
/// Should a leaf ever need one, the order is view_renderer before leaf — matching init_declare, which prewarms the same leaves.
class view_renderer : public sg::render_routine<view_renderer>
{
public:
    /// Path-traces every view in `def` into its own transient rgba16f target, then opens a raster scope on `output` and blits the first view across it.
    /// Records the whole frame — trace *and* blit — onto `cmd`.
    /// The caller submits (and presents) it.
    /// A no-op if `def` has no views.
    ///
    /// One command list end to end: sg transitions each transient target from the trace's UAV write to the blit's sampled read on its own.
    /// `output` names the format the blit's pipeline is built for.
    /// Each view needs at least one triangle-mesh item.
    /// Multi-view compositing into several outputs is the next seam — today only the first view reaches `output`.
    static void execute(sg::command_list& cmd,
                        viewer_definition const& def,
                        scene_resources& resources,
                        sg::color_target const& output);

protected:
    /// No shaders of its own; it warms the leaf routines (pathtrace + blit) so their compiles start early.
    void init_declare(sg::context& ctx) override;

private:
    /// Per-view resources that persist across frames — temporal accumulators, history.
    /// Empty this slice; the map and its view_id key exist so accumulation drops in without reshaping the renderer.
    struct persistent_view_resources
    {
    };

    cc::map<view_id, persistent_view_resources> _persistent;
};
} // namespace sv
