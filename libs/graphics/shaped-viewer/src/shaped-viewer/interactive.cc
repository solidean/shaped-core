#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/capture.hh>
#include <shaped-viewer/context.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/impl/web_page.hh>
#include <shaped-viewer/interactive.hh>
#include <shaped-viewer/shader_library.hh>

namespace sv
{
namespace
{
/// Folds a capture request into the config the viewer is brought up with.
///
/// This is the whole reason capture costs an example nothing: the request decides headless and the resolution before
/// the viewer exists, so a body written for the interactive case is also the body that produces a reference image.
void apply_capture_config(viewer_config& config, sr::capture_request const& req)
{
    config.headless = true;
    config.width = req.size[0];
    config.height = req.size[1];
}

/// The request the environment asks for, folded into `config` when it is active.
///
/// An active request with nowhere to write is a caller error rather than a silent no-op — a tool that forgot the path
/// would otherwise get a run that looked like it worked.
[[nodiscard]] sr::capture_request apply_environment_capture(viewer_config& config)
{
    auto req = sr::capture_request::from_environment();
    if (!req.active)
        return req;

    if (req.output_path.empty())
    {
        CC_LOG_ERROR("capture: {} is set but {} names no file — running interactively instead",
                     sr::capture_request_env_var, sr::capture_output_env_var);
        return sr::capture_request{};
    }

    apply_capture_config(config, req);
    return req;
}

/// How many frames an awaited loop under node draws before it stops: a smoke run, since nothing there can close it.
constexpr int node_frames = 300;

/// How many frames a page draws before its loading overlay may go, so a first frame drawn before any pipeline was
/// asked for does not count as the finished image.
constexpr int frames_before_loaded = 3;

/// Finishes an abandoned loop's viewer and then destroys it, on a context whose viewer cannot drain in its destructor.
cc::shared_async<cc::unit> finish_abandoned(cc::unique_ptr<viewer> v)
{
    co_await v->finish();
}

} // namespace

struct async_frames::state
{
    cc::string id;
    viewer_config config;

    /// The context the viewer draws on: the caller's from the start, or the acquired one once the viewer is up.
    sg::context* ctx = nullptr;

    cc::unique_ptr<viewer> owned;
    bool started = false;
    bool done = false;

    /// The frame `next()` handed out, which the next call ends; null between frames.
    frame* current = nullptr;

    /// Whether the viewer draws into the page's canvas, and whether the page's loading overlay is gone.
    bool on_page = false;
    bool loaded = false;

    /// What the loading overlay says now, so the page is written only when it changes.
    char const* status = nullptr;

    /// Frames handed out so far, and how many a run stops after; 0 for no limit.
    int frames = 0;
    int frame_limit = 0;
};

frame_range interactive(cc::string_view id, viewer_config config)
{
    auto req = apply_environment_capture(config);

    // The range owns the viewer, so the loop is the viewer's whole lifetime and a caller keeps no handle on it.
    auto* const v = new viewer(viewer::create(id, cc::move(config)));
    if (req.active)
        v->install_capture(cc::move(req));
    v->begin_frames();
    return frame_range::owning(v);
}

frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config)
{
    auto req = apply_environment_capture(config);

    auto* const v = new viewer(viewer::create(ctx, id, cc::move(config)));
    if (req.active)
        v->install_capture(cc::move(req));
    v->begin_frames();
    return frame_range::owning(v);
}

frame_range interactive(cc::string_view id, viewer_config config, sr::capture_request capture)
{
    CC_ASSERT(capture.active && !capture.output_path.empty(), "an explicit capture must be active and name its output");
    apply_capture_config(config, capture);

    auto* const v = new viewer(viewer::create(id, cc::move(config)));
    v->install_capture(cc::move(capture));
    v->begin_frames();
    return frame_range::owning(v);
}

frame_range interactive(sg::context& ctx, cc::string_view id, viewer_config config, sr::capture_request capture)
{
    CC_ASSERT(capture.active && !capture.output_path.empty(), "an explicit capture must be active and name its output");
    apply_capture_config(config, capture);

    auto* const v = new viewer(viewer::create(ctx, id, cc::move(config)));
    v->install_capture(cc::move(capture));
    v->begin_frames();
    return frame_range::owning(v);
}
} // namespace sv

namespace sv
{
async_frames interactive_async(cc::string_view id, viewer_config config)
{
    auto s = cc::make_unique<async_frames::state>();
    s->id = cc::string(id);
    s->config = cc::move(config);
    return async_frames(cc::move(s));
}

async_frames interactive_async(sg::context& ctx, cc::string_view id, viewer_config config)
{
    auto s = cc::make_unique<async_frames::state>();
    s->id = cc::string(id);
    s->config = cc::move(config);
    s->ctx = &ctx;
    return async_frames(cc::move(s));
}

async_frames::async_frames(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}

async_frames::async_frames(async_frames&&) noexcept = default;

async_frames& async_frames::operator=(async_frames&& o) noexcept
{
    if (this != &o)
    {
        if (_state != nullptr)
            abandon(*_state);
        _state = cc::move(o._state);
    }
    return *this;
}

async_frames::~async_frames()
{
    if (_state != nullptr)
        abandon(*_state);
}

void async_frames::abandon(state& s)
{
    if (s.owned == nullptr)
        return;
    s.owned->request_close();
    s.current = nullptr;
    if (s.ctx->execution() == sg::execution_model::never_block)
        (void)s.ctx->backlog.start(finish_abandoned(cc::move(s.owned)));
    else
        s.owned = nullptr; // its destructor presents the open frame and drains
}

cc::shared_async<frame*> async_frames::next()
{
    CC_ASSERT(_state != nullptr, "next() on a moved-from loop");
    return advance(*_state);
}

cc::shared_async<frame*> async_frames::advance(state& s)
{
    if (s.done)
        co_return nullptr;

    if (!s.started)
    {
        s.started = true;
        auto acquired_ctx = sg::context_handle();
        if (s.ctx == nullptr)
        {
            auto const acquiring = acquire_viewer_context_async();
            auto const& acquired = co_await acquiring;
            if (acquired.has_error())
                co_await cc::async_fail(cc::format("{}: {}", s.id, acquired.error().to_string()));
            acquired_ctx = acquired.value();
        }

        // In a page, the viewer draws where the page shows it, unless the caller said where.
        s.on_page = impl::page_present() && !s.config.headless && !s.config.surface.has_value();
        if (s.on_page)
        {
            impl::install_page_input();
            s.config.surface = sg::swapchain_description{.window = {.platform = sg::window_platform::web_canvas,
                                                                    .handle = (void*)impl::page_canvas_selector,
                                                                    .client_size = impl::page_canvas_size()}};
        }
#ifdef __EMSCRIPTEN__
        else if (!impl::page_present() && !s.config.surface.has_value())
        {
            // Under node there is nothing to draw into and nobody to close it.
            s.config.headless = true;
            s.frame_limit = node_frames;
        }
#endif
        auto req = apply_environment_capture(s.config);

        // Brought up on the context acquired above, which `acquire_viewer_context` now hands back too.
        auto created = s.ctx != nullptr ? viewer::try_create(*s.ctx, s.id, cc::move(s.config))
                                        : viewer::try_create(s.id, cc::move(s.config));
        if (created.has_error())
            co_await cc::async_fail(cc::format("{}: {}", s.id, created.error().to_string()));
        if (s.ctx == nullptr)
            s.ctx = acquired_ctx.get();
        s.owned = cc::make_unique<viewer>(cc::move(created.value()));
        if (req.active)
            s.owned->install_capture(cc::move(req));
        s.owned->begin_frames();
    }

    auto& v = *s.owned;
    if (s.current != nullptr)
    {
        auto& f = *s.current;
        s.current = nullptr;
        if (s.on_page && !s.loaded)
        {
            // Shaders compile and textures stream after the frame that asked for them, so the image stands once
            // neither is owed and the resource managers have nothing left to do to it.
            auto compiling = s.ctx->backlog.outstanding_count();
            if (auto const library = acquire_shader_library(); library.has_value())
                compiling += library.value()->backlog().outstanding_count();
            auto const streaming = f.streaming_resources() + f.pending_resource_work();
            if (s.frames >= frames_before_loaded && compiling == 0 && streaming == 0)
            {
                s.loaded = true;
                impl::set_page_loaded();
            }
            else if (char const* const status = compiling > 0 ? "compiling shaders" : "streaming"; status != s.status)
            {
                s.status = status;
                impl::set_page_status(status);
            }
        }
        co_await v.end_frame_async();
    }

    while (true)
    {
        if (s.on_page)
        {
            co_await impl::next_animation_frame();
            for (auto& e : impl::take_page_input())
                v.feed_input(cc::move(e));
            if (auto const size = impl::take_page_resize(); size.has_value())
                v.set_surface_size(size.value());
        }
        if (s.frame_limit > 0 && s.frames >= s.frame_limit)
            v.request_close();

        if (!v.is_running())
        {
            co_await v.finish();
            s.owned = nullptr;
            s.done = true;
            co_return nullptr;
        }

        // A closed frame -- a canvas with no size -- submits nothing, and is waited past as any other.
        auto& f = v.begin_frame_unrecorded();
        if (f.is_open())
        {
            s.current = &f;
            ++s.frames;
            co_return &f;
        }
        co_await v.end_frame_async();
    }
}
} // namespace sv
