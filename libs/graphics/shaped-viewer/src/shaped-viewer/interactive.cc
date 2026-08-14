#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/interactive.hh>

namespace sv
{
frame_range interactive(cc::string_view id, viewer_config config)
{
    // The range owns the viewer, so the loop is the viewer's whole lifetime and a caller keeps no handle on it.
    auto* const v = new viewer(viewer::create(id, cc::move(config)));
    v->begin_frames();
    return frame_range::owning(v);
}

frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config)
{
    auto* const v = new viewer(viewer::create(ctx, id, cc::move(config)));
    v->begin_frames();
    return frame_range::owning(v);
}
} // namespace sv
