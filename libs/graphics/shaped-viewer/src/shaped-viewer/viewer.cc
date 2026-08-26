#include <clean-core/common/asserts.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/input.hh>
#include <shaped-rendering/shaders.hh> // sr::shader_package (blit)
#include <shaped-rendering/window.hh>
#include <shaped-viewer/context.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/fwd.hh> // std::unique_ptr, for the sg::command_list held across a frame
#include <shaped-viewer/impl/view_state.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/rendering/shaders.hh> // sv::shader_package (path tracer)
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/viewer_renderer.hh>
#include <shaped-viewer/resources/resource_managers.hh>
#include <shaped-viewer/shader_library.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/view/viewer_definition.hh>
#include <shaped-viewer/viewer.hh>

#include <chrono>

namespace sv
{
namespace
{
/// The color the window is cleared to before the views are composited (fills the gaps between them).
constexpr tg::vec4f clear_color = tg::vec4f(0.02f, 0.02f, 0.03f, 1.0f);

/// The cursor position an event carries, if it carries one.
/// A key event does not, which is why the viewer remembers the last one it saw.
[[nodiscard]] cc::optional<tg::pos2f> cursor_of(sr::input_event const& e)
{
    using result_t = cc::optional<tg::pos2f>;

    return e.payload.visit(                                                     //
        [](sr::mouse_move_event const& m) { return result_t(m.cursor_pos); },   //
        [](sr::mouse_button_event const& b) { return result_t(b.cursor_pos); }, //
        [](sr::mouse_wheel_event const& w) { return result_t(w.cursor_pos); },  //
        [](sr::key_event const&) { return result_t(); },                        //
        [](sr::text_event const&) { return result_t(); });
}

[[nodiscard]] bool contains(tg::aabb2i const& r, tg::pos2f p)
{
    auto const x = i32(p[0]);
    auto const y = i32(p[1]);
    return x >= r.min[0] && x < r.max[0] && y >= r.min[1] && y < r.max[1];
}

/// The leaf naming `view` as its first source, or `invalid_node`.
[[nodiscard]] layout_node_id find_leaf_of(layout_tree const& tree, view_index view)
{
    for (auto i = u32(0); i < tree.nodes.size(); ++i)
    {
        auto const& n = tree.nodes[i];
        if (n.kind == layout_kind::leaf && !n.leaf.views.empty() && n.leaf.views[0] == view)
            return layout_node_id(i);
    }
    return invalid_node;
}

/// Unhooks `node` from whichever container holds it; false when nothing does (it is already a root).
bool detach_from_parent(layout_tree& tree, layout_node_id node)
{
    for (auto& n : tree.nodes)
    {
        for (auto i = isize(0); i < n.children.size(); ++i)
        {
            if (n.children[i] != node)
                continue;

            n.children.remove_at(i);
            return true;
        }
    }
    return false;
}

/// How far one wheel tick magnifies, and how far the zoom may go.
/// The cap is what keeps a zoomed view from collapsing onto a sub-texel window nothing can be read out of.
constexpr float zoom_per_tick = 1.2f;
constexpr float max_zoom = 64.0f;

/// The modifiers a caller holds to drag a view out of the layout.
/// Ctrl again, so one modifier covers "act on the layout" while the bare buttons keep acting on the camera.
constexpr sr::key_modifiers move_modifiers = sr::key_modifiers::ctrl;

/// The modifiers a caller holds to zoom into a view rather than move its camera.
/// Ctrl+wheel is the near-universal binding for exactly this, and it leaves the plain wheel to the camera.
constexpr sr::key_modifiers zoom_modifiers = sr::key_modifiers::ctrl;
} // namespace

struct viewer::impl
{
    // Only `resources` must be constructed in place (its managers hold a context reference, so it is not
    // assignable); everything else try_create fills in after make_unique.
    impl(sg::context& c, gpu_resource_manager res) : ctx(&c), resources(cc::move(res)) {}

    view_id id;
    viewer_config config;

    /// Set only when the viewer acquired its own context; a caller-supplied one is theirs to outlive us.
    /// Either way `ctx` is what everything else uses.
    sg::context_handle owned_ctx;
    sg::context* ctx = nullptr;

    // window_system before window so window (which must not outlive it) is destroyed first.
    cc::unique_ptr<sr::window_system> window_system;
    cc::unique_ptr<sr::window> window;
    sg::swapchain_handle swapchain;


    gpu_resource_manager resources;

    u64 frame_index = 0;
    bool stopped = false; // device lost

    // Both stamped in next_frame: `start_time` gives the frame its elapsed seconds, `last_frame_time` its delta.
    // last_frame_time only advances on frames that were actually drawn, so a skipped (minimized) stretch lands in
    // one delta on resume rather than vanishing.
    std::chrono::steady_clock::time_point start_time = {};
    std::chrono::steady_clock::time_point last_frame_time = {};

    // the frame currently being recorded (one at a time)
    sg::render_target_view current_backbuffer;
    std::unique_ptr<sg::command_list> current_cmd; // sg command lists are std::unique_ptr, not cc

    // The frame begin_frame handed out, live until end_frame presents it — so the caller holds a reference rather
    // than a frame whose destructor they then have to reason about.
    // Closed between the two, and unused by frames(), whose frame belongs to the loop variable.
    frame current_frame;

    // Everything this viewer's views keep across frames: cameras, controllers, placements and textures alike.
    // Handed to the renderers rather than reached out of them — see view/view_store.hh.
    sv::view_store views;

    // Where the cursor was at the last event that reported one, so a key event still routes somewhere.
    tg::pos2f last_cursor_pos = tg::pos2f(0, 0);

    // A wheel event carries no modifiers, unlike key and button events, so they are remembered from those.
    sr::key_modifiers last_modifiers = sr::key_modifiers::none;

    // The view holding a drag, so motion keeps reaching it once the cursor leaves its rect.
    cc::optional<view_id> active_view;

    // The view being dragged out of the layout, and where inside it the cursor grabbed on.
    // The offset is what keeps the pane from snapping its corner to the cursor on the first motion.
    cc::optional<view_id> moving_view;
    tg::vec2f move_grab_offset = tg::vec2f(0, 0);

    // Last frame's leaves, in window space and with the links that order them.
    // Routing runs before the next frame is authored, so last frame's answer is the only one there is.
    cc::vector<hit_region> last_hit_regions;
};

cc::result<viewer> viewer::try_create(cc::string_view id_str, viewer_config config)
{
    auto ctx_r = acquire_viewer_context();
    if (ctx_r.has_error())
        return cc::error(cc::move(ctx_r.error())); // any_error is move-only

    auto handle = ctx_r.value(); // acquire_viewer_context already refused a null one
    auto v = try_create(*handle, id_str, cc::move(config));
    if (v.has_error())
        return v;

    // Take the reference count only once the viewer exists, so a failed bring-up leaves nothing holding the context.
    v.value()._impl->owned_ctx = cc::move(handle);
    return v;
}

cc::result<viewer> viewer::try_create(sg::context& ctx, cc::string_view id_str, viewer_config config)
{
    auto ws_r = sr::window_system::try_create();
    if (ws_r.has_error())
        return cc::error("shaped-viewer: no window backend / display available");
    auto ws = cc::move(ws_r.value());

    // An unset title takes the viewer's own name, so naming a viewer is usually all a caller has to do.
    auto const title = config.title.has_value() ? cc::string_view(config.title.value()) : display_name_of(id_str);

    auto win_r = ws->try_create_window({.title = title, .width = config.width, .height = config.height});
    if (win_r.has_error())
        return cc::error("shaped-viewer: could not create a window");
    auto win = cc::move(win_r.value());

    // Ray tracing must be supported to build the meshes' BLAS; fail cleanly rather than asserting later.
    {
        auto probe = ctx.create_command_list();
        auto const rt_supported = probe->raytracing.is_supported();
        ctx.drop_command_list(cc::move(probe));
        if (!rt_supported)
            return cc::error("shaped-viewer: the rendering context reports no ray-tracing support");
    }

    auto sc_r = ctx.try_create_swapchain({.native_window_handle = win->native_window_handle(),
                                          .buffer_count = config.buffer_count,
                                          .format = sg::pixel_format::bgra8_unorm});
    if (sc_r.has_error())
        return cc::error("shaped-viewer: could not create a swapchain for the window");
    auto sc = sc_r.value();

    // The library is process-wide rather than the viewer's, because a *generated* material permutation is compiled from the
    // render path, which has no viewer to reach back to.
    // A caller wanting their own registers it through set_acquire_shader_library before the first viewer.
    if (acquire_shader_library().has_error())
        return cc::error("shaped-viewer: could not bring up a shader library");

    auto im = cc::make_unique<viewer::impl>(ctx, gpu_resource_manager::create(ctx, config.resources));
    im->id = view_id::from_string(id_str);
    im->config = cc::move(config);
    im->window_system = cc::move(ws);
    im->window = cc::move(win);
    im->swapchain = cc::move(sc);

    im->start_time = std::chrono::steady_clock::now();
    return viewer(cc::move(im));
}

viewer viewer::create(cc::string_view id, viewer_config config)
{
    auto r = try_create(id, cc::move(config));
    CC_ASSERT(!r.has_error(), "viewer::create failed — use try_create to handle the error");
    return cc::move(r.value());
}

viewer viewer::create(sg::context& ctx, cc::string_view id, viewer_config config)
{
    auto r = try_create(ctx, id, cc::move(config));
    CC_ASSERT(!r.has_error(), "viewer::create failed — use try_create to handle the error");
    return cc::move(r).value();
}

viewer::viewer(cc::unique_ptr<impl> im) : _impl(cc::move(im))
{
}
viewer::viewer(viewer&&) noexcept = default;
viewer& viewer::operator=(viewer&&) noexcept = default;

viewer::~viewer()
{
    if (_impl == nullptr)
        return;
    try
    {
        // A loop that left through a break still holds a frame here, and its backbuffer and command list have to be
        // handed back before anything drains — so it presents, exactly as if end_frame had been reached.
        end_frame();

        _impl->ctx->advance_epoch_and_wait_for_idle();
    }
    catch (sg::device_lost_exception const&)
    {
        // tearing down after a device loss — nothing left to drain
    }
}

void viewer::begin_frames()
{
    _impl->frame_index = 0;
    _impl->start_time = std::chrono::steady_clock::now();
    _impl->last_frame_time = _impl->start_time;
}

bool viewer::is_running() const
{
    auto const& im = *_impl;
    if (im.stopped || im.window == nullptr)
        return false;
    return !im.window->is_close_requested() && !im.window_system->is_quit_requested();
}

gpu_resource_manager& viewer::resources()
{
    return _impl->resources;
}

sv::impl::view_state& viewer::state_of(view_id id)
{
    return _impl->views.get_or_create(id);
}

void viewer::begin_move(tg::pos2f window_point)
{
    auto& im = *_impl;

    auto const hit = pick_hit_region(im.last_hit_regions, window_point);
    if (hit == invalid_hit_region)
        return;

    auto const& region = im.last_hit_regions[hit];
    auto* const st = im.views.get_ptr(region.id);
    if (st == nullptr || !st->movable_last_frame)
        return;

    // Lifting keeps the size the view already had, so the pane does not jump on the first motion.
    auto const w = f32(im.window->width() > 0 ? im.window->width() : 1);
    auto const h = f32(im.window->height() > 0 ? im.window->height() : 1);

    st->placement.size = tg::vec2f(f32(region.window_rect.max[0] - region.window_rect.min[0]) / w,
                                   f32(region.window_rect.max[1] - region.window_rect.min[1]) / h);
    st->placement.position_offset = tg::vec2i(0, 0);
    st->placement.size_offset = tg::vec2i(0, 0);

    im.moving_view = region.id;
    im.move_grab_offset
        = tg::vec2f(window_point[0] - f32(region.window_rect.min[0]), window_point[1] - f32(region.window_rect.min[1]));
}

void viewer::move_to(tg::pos2f window_point)
{
    auto& im = *_impl;
    if (!im.moving_view.has_value())
        return;

    auto* const st = im.views.get_ptr(im.moving_view.value());
    if (st == nullptr)
        return;

    auto const w = f32(im.window->width() > 0 ? im.window->width() : 1);
    auto const h = f32(im.window->height() > 0 ? im.window->height() : 1);

    // A fraction of the window, not a pixel rect: the caller's tree is rebuilt every frame, so the only thing that can
    // survive it is a placement the layout re-resolves from scratch.
    st->placement.position = tg::pos2f(cc::clamp((window_point[0] - im.move_grab_offset[0]) / w, -1.0f, 1.0f),
                                       cc::clamp((window_point[1] - im.move_grab_offset[1]) / h, -1.0f, 1.0f));
    st->placement_seeded = true;
}

void viewer::zoom_at(tg::pos2f window_point, float ticks)
{
    auto& im = *_impl;

    auto const hit = pick_hit_region(im.last_hit_regions, window_point);
    if (hit == invalid_hit_region)
        return;

    auto const& region = im.last_hit_regions[hit];
    auto* const st = im.views.get_ptr(region.id);
    if (st == nullptr || st->composite.raw() == nullptr)
        return; // a view with no image yet has nothing to magnify into

    auto const resolution = tg::vec2i(st->composite.width(), st->composite.height());
    if (resolution[0] <= 0 || resolution[1] <= 0)
        return;

    // The region maps this view's texels to window pixels, so inverting it says which texel the cursor is over.
    // That mapping already accounts for the zoom in force, which is what makes the point under the cursor stay put.
    auto const texel_x = (window_point[0] - region.offset[0]) / (region.scale[0] != 0 ? region.scale[0] : 1.0f);
    auto const texel_y = (window_point[1] - region.offset[1]) / (region.scale[1] != 0 ? region.scale[1] : 1.0f);

    st->zoom = cc::clamp(st->zoom * tg::pow(zoom_per_tick, ticks), 1.0f, max_zoom);
    st->zoom_center = tg::pos2f(cc::clamp(texel_x / f32(resolution[0]), 0.0f, 1.0f), //
                                cc::clamp(texel_y / f32(resolution[1]), 0.0f, 1.0f));
}

void viewer::route_input()
{
    auto& im = *_impl;

    for (auto const& e : im.window_system->events())
    {
        if (e.window != im.window.get())
            continue;
        if (auto const p = cursor_of(e); p.has_value())
            im.last_cursor_pos = p.value();
        if (e.is_key())
            im.last_modifiers = e.as_key().modifiers;
        if (e.is_mouse_button())
            im.last_modifiers = e.as_mouse_button().modifiers;

        // Ctrl+wheel magnifies the image under the cursor instead of moving a camera, and is consumed here so the
        // controller never also zooms.
        // It is a pure readout: nothing it writes reaches a trace, so a converged image stays converged while it is
        // inspected.
        if (e.is_mouse_wheel() && sr::has_all(im.last_modifiers, zoom_modifiers))
        {
            zoom_at(im.last_cursor_pos, e.as_mouse_wheel().delta[1]);
            continue;
        }

        // Ctrl + left-drag lifts a view the caller offered, and is consumed so the controller never also orbits it.
        if (e.is_mouse_button() && e.as_mouse_button().button == sr::mouse_button::left)
        {
            auto const& b = e.as_mouse_button();
            if (b.is_down && sr::has_all(im.last_modifiers, move_modifiers))
            {
                begin_move(im.last_cursor_pos);
                if (im.moving_view.has_value())
                    continue;
            }
            else if (!b.is_down && im.moving_view.has_value())
            {
                im.moving_view = {};
                continue;
            }
        }
        if (im.moving_view.has_value())
        {
            move_to(im.last_cursor_pos);
            continue;
        }

        // A live drag keeps its view even once the cursor leaves the rect; otherwise the frontmost leaf under it wins.
        auto* st = im.active_view.has_value() ? im.views.get_ptr(im.active_view.value()) : nullptr;
        auto owner = im.active_view;
        if (st == nullptr)
        {
            // Painter's order over the plan's regions, not a scan of every view's rect: a nested view and the wrapper
            // holding it cover the same pixels, and only the region links say which of them is actually in front.
            auto const hit = pick_hit_region(im.last_hit_regions, im.last_cursor_pos);
            if (hit != invalid_hit_region)
            {
                owner = im.last_hit_regions[hit].id;
                st = im.views.get_ptr(owner.value());
            }
        }

        // A caller that set this view's camera last frame owns it, so the controller must not fight over it.
        if (st == nullptr || st->camera_owned_last_frame)
            continue;

        if (st->controller.handle(e))
            st->camera = st->controller.camera();

        // The drag holder lives here rather than on the view: only one view can hold it, and it must survive the
        // cursor leaving that view's rect.
        im.active_view = st->controller.is_dragging() ? owner : cc::optional<view_id>();
    }
}

frame viewer::acquire_frame()
{
    auto& im = *_impl;
    im.window_system->poll_events();

    // Advanced before authoring, because seeding and the hit-test below read it — and still before anything resolves a
    // texture, which is all its reclaim needs.
    im.views.begin_frame(u64(im.ctx->current_epoch()));
    route_input();

    if (im.window->is_close_requested() || im.window_system->is_quit_requested())
        return frame{}; // closed — the loop stops on is_running()

    if (im.window->is_minimized())
        return frame{}; // closed — skip this frame, the window is still open

    try
    {
        im.current_backbuffer = im.swapchain->acquire_backbuffer();
    }
    catch (sg::device_lost_exception const&)
    {
        im.stopped = true;
        return frame{};
    }
    im.current_cmd = im.ctx->create_command_list();

    // Sampled once, here, so every view in the frame sees the same instant.
    // The first drawn frame has no predecessor, so its delta is 0 rather than the loop's start-up cost, and it is also
    // where the elapsed clock starts — a hand-driven loop opens with begin_frame and nothing else.
    auto const now = std::chrono::steady_clock::now();
    if (im.frame_index == 0)
        im.start_time = now;
    auto const delta = im.frame_index == 0 ? 0.0 : std::chrono::duration<double>(now - im.last_frame_time).count();
    im.last_frame_time = now;
    ++im.frame_index;

    auto f = frame{};
    f._viewer = this;
    f._size = im.current_backbuffer.size();
    f._seconds = std::chrono::duration<double>(now - im.start_time).count();
    f._delta_seconds = delta;
    f._id = im.frame_index;
    f._open = true;
    return f;
}

frame& viewer::begin_frame()
{
    auto& im = *_impl;
    CC_ASSERT(!im.current_frame._open, "the previous frame was never ended — every frame begin_frame opens needs its "
                                       "end_frame");

    // The outer frame bracket, closed in end_frame — everything a frame does nests under this one span.
    // After the assert above, so the misuse it catches does not also open a second span.
    CC_RECORD_SCOPE_BEGIN("sv.frame");

    // A closed frame holds no backbuffer and no command list, so overwriting one strands nothing.
    im.current_frame = acquire_frame();
    return im.current_frame;
}

void viewer::end_frame()
{
    // present() already no-ops on a frame that is closed or was never begun, so the minimized path needs no second rule.
    // Dropping the frame afterwards is what makes "is a frame open" a question the stored frame itself answers.
    _impl->current_frame.present();
    _impl->current_frame = frame{};

    // Closed LAST, so the present is inside the frame it belongs to rather than after it.
    CC_RECORD_SCOPE_END("sv.frame");
}

frame_range viewer::frames()
{
    begin_frames();
    return frame_range(this);
}

void viewer::request_close()
{
    if (_impl == nullptr || _impl->window == nullptr)
        return;

    // Routed through the window so it is the same signal the close button raises, and is_running() needs no second condition.
    _impl->window->request_close();
}

void viewer::finish_frame(frame& f)
{
    auto& im = *_impl;
    if (!f._open)
        return;

    // The frame's authored views become the definition verbatim, and the frame's layout tree becomes the one layer of
    // a synthetic root view.
    // The root is appended last, so every leaf's existing view index stays valid.
    auto def = viewer_definition{};
    def.views = f._views;
    def.nodes = f._nodes;

    for (auto& v : def.views)
    {
        auto& st = im.views.get_or_create(v.id);

        // A caller that set a camera this frame owns it; otherwise the view renders from whatever it was orbited to.
        if (!st.camera_owned_this_frame)
            v.camera = st.camera;
        else
            st.camera = v.camera; // so a later hand-off to the controller starts where the caller left off

        // Routing runs before the next frame is authored, so this frame's answer becomes what it reads.
        st.camera_owned_last_frame = st.camera_owned_this_frame;
        st.camera_owned_this_frame = false;
        st.movable_last_frame = st.movable_this_frame;
    }

    // The root exists only to own the frame's layout; it holds no scene of its own and its target is the backbuffer.
    //
    // Which layout that is depends on what the caller put on the window's own root view.
    // A window view carrying nothing but a layout is flattened away — the root adopts that layout directly, so the
    // common case costs no second texture.
    // A window view carrying content of its own — `f.add_scene()`, the shorthand that names no view at all — cannot be:
    // it has to render into its own texture, so the root gets a layout of one leaf naming it.
    auto root = view_data{};
    root.id = im.id;

    auto const window_draws_itself = [&]
    {
        if (f._windows.empty())
            return false;
        for (auto const& l : def[f._windows.front()].layers)
            if (l.kind != layer_kind::layout)
                return true;
        return false;
    }();

    if (window_draws_itself)
    {
        auto const window_view = f._windows.front();

        // Appended, so every leaf the caller already filled keeps its node id.
        auto const node = def.nodes.add_container(invalid_node);
        auto leaf = layout_leaf{};
        leaf.views.push_back(window_view);
        (void)def.nodes.add_leaf(node, cc::move(leaf));
        root.layers.push_back({.kind = layer_kind::layout, .blend = layer_blend::replace, .root_node = node});
    }
    else if (!def.nodes.empty())
        root.layers.push_back({.kind = layer_kind::layout, .blend = layer_blend::replace, .root_node = layout_node_id(0)});

    def.root_view = view_index(def.views.size());
    def.views.push_back(cc::move(root));

    // Lift every view a drag has placed: its leaf moves out of wherever the caller put it and under a relative node on
    // the root's own layout, so it floats over the whole window rather than inside its original container.
    // Only views the caller re-offered this frame move, and the leaf itself is untouched — so the view keeps its id,
    // its camera and its converged image across the lift.
    if (!def.nodes.empty())
    {
        auto const root_layout = [&]() -> layout_node_id
        {
            for (auto const& l : def[def.root_view].layers)
                if (l.kind == layer_kind::layout)
                    return l.root_node;
            return invalid_node;
        }();

        if (root_layout != invalid_node)
        {
            for (auto i = u32(0); i < def.views.size(); ++i)
            {
                auto const view = view_index(i);
                auto const& st = im.views.get_or_create(def[view].id);
                if (!st.movable_this_frame || !st.placement_seeded)
                    continue;

                auto const leaf = find_leaf_of(def.nodes, view);
                if (leaf == invalid_node || !detach_from_parent(def.nodes, leaf))
                    continue;

                auto const floated = def.nodes.add_relative(root_layout, st.placement);
                def.nodes[floated].children.push_back(leaf);
            }
        }
    }

    // Cleared only now, since the lift above is the last thing to read it — a caller who stops offering a view stops
    // being able to drag it from the next frame on, while whatever a previous drag already did to it survives.
    for (auto const& v : def.views)
        im.views.get_or_create(v.id).movable_this_frame = false;

    // The zoom lives on the view it magnifies, but the *leaf* is what samples — so resolve it across before planning.
    // Passing it as data keeps `build_render_plan` a pure function, and keeps the zoom out of every trace hash.
    for (auto& node : def.nodes.nodes)
    {
        if (node.kind != layout_kind::leaf || !node.leaf.allow_zoom || node.leaf.views.empty())
            continue;
        if (isize(u32(node.leaf.views[0])) >= def.views.size())
            continue;

        auto const& st = im.views.get_or_create(def[node.leaf.views[0]].id);
        node.leaf.zoom = st.zoom;
        node.leaf.zoom_center = st.zoom_center;
    }

    // What the store already holds for each view, so the plan can decide what refreshes.
    // Read off the composite texture rather than a field stamped beside it, so "the plan believes this can be
    // re-presented" and "there is something to re-present" cannot drift apart.
    auto history = view_history{};
    for (auto const& v : def.views)
    {
        auto const& st = im.views.get_or_create(v.id);
        auto const exists = st.composite.raw() != nullptr;
        history.entries[v.id]
            = {.exists = exists,
               .resolution = exists ? tg::vec2i(st.composite.width(), st.composite.height()) : tg::vec2i(0, 0),
               .last_refresh_frame = st.last_refresh_frame};
    }

    auto const plan = build_render_plan(def, f._size, f._id, history);

    // Where each view actually landed in window space, which is what the next frame hit-tests against.
    // It comes from the plan rather than a layout solve, because a nested leaf's rect lives in its parent's texture
    // space and only the plan has carried it all the way up.
    im.last_hit_regions = plan.hit_regions;

    // The one piece of history with no texture to read it back off.
    for (auto const& target : plan.targets)
        if (target.refresh)
            im.views.get_or_create(target.id).last_refresh_frame = f._id;

    try
    {
        // Reclaim stale / over-budget resources and advance to this frame's epoch, before any view resolves its ids or
        // reaches for its accumulator.
        // The view store already advanced on the same epoch, back in next_frame.
        im.resources.advance_to(im.ctx->current_epoch());

        // With no views authored this places nothing and the clear alone lands, so the window is never left with
        // stale contents.
        viewer_renderer::execute(*im.current_cmd, def, plan, im.resources, im.views,
                                 im.current_backbuffer.cleared(clear_color));
        im.ctx->submit_command_list_and_present(*im.swapchain, cc::move(im.current_cmd));
        im.ctx->advance_epoch(im.swapchain->buffer_count());
    }
    catch (sg::device_lost_exception const&)
    {
        im.stopped = true;
    }

    im.current_cmd = nullptr;
    im.current_backbuffer = sg::render_target_view{};
}
} // namespace sv
