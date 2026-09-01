#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/refs.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/resident_mesh.hh>

namespace sv
{
// ---- mesh_ref / light_ref --------------------------------------------------------------------------------

scene_item& mesh_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer].items[_item];
}

void mesh_ref::transform(tg::affine_transform3f const& t)
{
    target().transform = t;
}

area_light& light_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer].area_lights[_light];
}

void light_ref::light(area_light const& l)
{
    target() = l;
}

// ---- scene_ref -------------------------------------------------------------------------------------------

layer& scene_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer];
}

mesh_ref scene_ref::add_mesh(sv::mesh const& mesh)
{
    // Every step of it is content-keyed, so a caller re-adding an unchanged mesh every frame pays lookups.
    auto& items = target().items;
    items.push_back(_frame->resources().acquire_scene_item(mesh));
    return mesh_ref(_frame, _view, _layer, u32(items.size() - 1));
}

mesh_ref scene_ref::add_mesh(sv::resident_mesh const& mesh)
{
    auto& items = target().items;
    items.push_back(_frame->resources().acquire_scene_item(mesh));
    return mesh_ref(_frame, _view, _layer, u32(items.size() - 1));
}

light_ref scene_ref::add_light(area_light const& light)
{
    auto& lights = target().area_lights;
    lights.push_back(light);
    return light_ref(_frame, _view, _layer, u32(lights.size() - 1));
}

void scene_ref::background(sv::background const& bg)
{
    target().background = bg;
}

void scene_ref::settings(render_settings const& s)
{
    target().settings = s;
}

// ---- leaf_ref --------------------------------------------------------------------------------------------

layout_leaf& leaf_ref::target() const
{
    return _frame->_nodes[_node].leaf;
}

view_ref leaf_ref::add_view(cc::string_view id)
{
    auto const index = _frame->add_view(id);
    target().views.push_back(index);
    return view_ref(_frame, index);
}

void leaf_ref::post_process(sv::post_process const& p)
{
    target().post_processes.push_back(p);
}

void leaf_ref::fit(fit_mode m)
{
    target().fit = m;
}

void leaf_ref::sampler(sampler_mode m)
{
    target().sampler = m;
}

void leaf_ref::allow_zoom(bool v)
{
    target().allow_zoom = v;
}

// ---- layout_ref ------------------------------------------------------------------------------------------

view_ref layout_ref::add_view(cc::string_view id)
{
    return leaf().add_view(id);
}

leaf_ref layout_ref::leaf()
{
    return leaf_ref(_frame, _frame->_nodes.add_leaf(_node, {}));
}

layout_ref layout_ref::rows(box_style style)
{
    return layout_ref(_frame, _frame->_nodes.add_container(_node, style, {.cols = 1}));
}

layout_ref layout_ref::columns(box_style style)
{
    return layout_ref(_frame, _frame->_nodes.add_container(_node, style, {.rows = 1}));
}

layout_ref layout_ref::grid(int cols, int rows, box_style style)
{
    return layout_ref(_frame, _frame->_nodes.add_container(_node, style, {.cols = cols, .rows = rows}));
}

layout_ref layout_ref::grid(grid_params params, box_style style)
{
    return layout_ref(_frame, _frame->_nodes.add_container(_node, style, params));
}

layout_ref layout_ref::auto_grid(box_style style, grid_params params)
{
    return layout_ref(_frame, _frame->_nodes.add_container(_node, style, params));
}

layout_ref layout_ref::relative(relative_placement placement, box_style style)
{
    return layout_ref(_frame, _frame->_nodes.add_relative(_node, placement, style));
}

void layout_ref::style(box_style const& s)
{
    _frame->_nodes[_node].style = s;
}

// ---- view_ref --------------------------------------------------------------------------------------------

view_data& view_ref::target() const
{
    return _frame->_views[u32(_view)];
}

view_id view_ref::id() const
{
    return target().id;
}

scene_ref view_ref::add_scene()
{
    // A traced layer writes no meaningful alpha, so it overwrites whatever sits below it rather than blending.
    auto& v = target();
    v.layers.push_back({.kind = layer_kind::scene_3d, .blend = layer_blend::replace});
    return scene_ref(_frame, _view, u32(v.layers.size() - 1));
}

layout_ref view_ref::open_layout(box_style style, grid_params params)
{
    auto& v = target();

    // A view holds one layout layer: asking again hands back the same tree, so two calls in a frame do not stack two
    // layouts on top of each other.
    for (auto const& l : v.layers)
        if (l.kind == layer_kind::layout)
            return layout_ref(_frame, l.root_node);

    auto const root = _frame->_nodes.add_container(invalid_node, style, params);
    target().layers.push_back({.kind = layer_kind::layout, .blend = layer_blend::replace, .root_node = root});
    return layout_ref(_frame, root);
}

layout_ref view_ref::layout_rows(box_style style)
{
    return open_layout(style, {.cols = 1});
}

layout_ref view_ref::layout_columns(box_style style)
{
    return open_layout(style, {.rows = 1});
}

layout_ref view_ref::layout_grid(int cols, int rows, box_style style)
{
    return open_layout(style, {.cols = cols, .rows = rows});
}

layout_ref view_ref::layout_grid(grid_params params, box_style style)
{
    return open_layout(style, params);
}

layout_ref view_ref::layout_auto_grid(box_style style, grid_params params)
{
    return open_layout(style, params);
}

void view_ref::camera(sv::camera const& cam)
{
    target().camera = cam;
    // Claiming the camera this frame is what keeps the built-in controller off this view.
    _frame->state_of(_view).camera_owned_this_frame = true;
}

void view_ref::initial_camera(sv::camera const& cam)
{
    auto& st = _frame->state_of(_view);
    if (!st.camera_seeded)
    {
        st.camera_seeded = true;
        st.camera = cam;
        st.controller.orbit = orbit_state::from_camera(cam, st.controller.orbit.distance);
    }
}

void view_ref::initial_orbit(orbit_state const& o)
{
    auto& st = _frame->state_of(_view);
    if (!st.camera_seeded)
    {
        st.camera_seeded = true;
        st.controller.orbit = o;
        st.camera = o.to_camera();
    }
}

void view_ref::initial_fps(fps_state const& pose)
{
    auto& st = _frame->state_of(_view);
    if (!st.camera_seeded)
    {
        st.camera_seeded = true;
        st.fly.pose = pose;
        st.camera = pose.to_camera();

        // The style has not been seeded yet either, and its seeding would re-derive this pose from the camera.
        // Marking it done here is what keeps the authored yaw and pitch rather than the round-trip's.
        st.style_seeded = true;
        st.style_last_frame = camera_style::fly;
    }
}

void view_ref::resolution(tg::vec2i r)
{
    auto& v = target();
    v.resolution = r;
    v.resolution_follows_layout = false;
}

void view_ref::refresh_rate(float rate)
{
    target().refresh.rate = rate;
}

void view_ref::movable(bool v)
{
    _frame->state_of(_view).movable_this_frame = v;
}

void view_ref::camera_style(sv::camera_style style)
{
    _frame->state_of(_view).style_this_frame = style;
}

void view_ref::display_name(cc::string_view name)
{
    _frame->state_of(_view).display_name = name;
}

cc::string_view view_ref::display_name() const
{
    return _frame->state_of(_view).display_name;
}

u32 view_ref::accumulated_frames() const
{
    return impl::min_accumulated_frames(_frame->state_of(_view));
}

bool view_ref::is_accumulation_converged(cc::optional<u32> frames) const
{
    return impl::is_accumulation_converged(_frame->state_of(_view), frames);
}

// ---- window_ref ------------------------------------------------------------------------------------------

view_ref window_ref::default_view() const
{
    return view_ref(_frame, _frame->_windows[_window]);
}
} // namespace sv
