#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-viewer/capture.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/interactive.hh>

namespace sv
{
namespace
{
/// Folds a capture request into the config the viewer is brought up with.
///
/// This is the whole reason capture costs an example nothing: the request decides headless and the resolution before
/// the viewer exists, so a body written for the interactive case is also the body that produces a reference image.
/// An active request with nowhere to write is a caller error rather than a silent no-op — a tool that forgot the path
/// would otherwise get a run that looked like it worked.
[[nodiscard]] capture_request apply_capture_config(viewer_config& config)
{
    auto req = capture_request::from_environment();
    if (!req.active)
        return req;

    if (req.output_path.empty() && !req.list_only)
    {
        CC_LOG_ERROR("capture: {} is set but {} names no file — running interactively instead", capture_request_env_var,
                     capture_output_env_var);
        return capture_request{};
    }

    config.headless = true;
    config.width = req.size[0];
    config.height = req.size[1];
    return req;
}

} // namespace

frame_range interactive(cc::string_view id, viewer_config config)
{
    auto req = apply_capture_config(config);

    // The range owns the viewer, so the loop is the viewer's whole lifetime and a caller keeps no handle on it.
    auto* const v = new viewer(viewer::create(id, cc::move(config)));
    if (req.active)
        v->install_capture(cc::move(req));
    v->begin_frames();
    return frame_range::owning(v);
}

frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config)
{
    auto req = apply_capture_config(config);

    auto* const v = new viewer(viewer::create(ctx, id, cc::move(config)));
    if (req.active)
        v->install_capture(cc::move(req));
    v->begin_frames();
    return frame_range::owning(v);
}
} // namespace sv
