#include <clean-core/common/asserts.hh>
#include <clean-core/common/utility.hh>     // cc::move
#include <shaped-rendering/slug_routine.hh> // sr::pack_rgba8
#include <shaped-rendering/text_layout.hh>
#include <shaped-viewer/drawing/annotation.hh>
#include <shaped-viewer/drawing/drawing.hh>
#include <shaped-viewer/drawing/font.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/refs.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/cross.hh>   // tg::cross, tg::dual
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot

namespace sv
{
namespace
{
/// A placement of drawing `index` of the acquired set `set`, with the scale folded into the axes.
[[nodiscard]] drawing_placement place_drawing(drawing_manager& drawings,
                                              drawing_set_id set,
                                              u32 index,
                                              tg::pos3f at,
                                              tg::vec3f x_axis,
                                              tg::vec3f y_axis,
                                              f32 scale,
                                              tg::vec4f tint,
                                              sv::corner from)
{
    return {.set = set,
            .page = drawings.page_of(set),
            .first_record = drawings.first_record(set, index),
            .record_count = drawings.record_count(set, index),
            .at = at,
            .x_axis = x_axis * scale,
            .y_axis = y_axis * scale,
            .tint = sr::pack_rgba8(tint),
            .from = from};
}

[[nodiscard]] drawing_placement place_2d(drawing_manager& drawings, drawing_set_id set, u32 index, instance_2d const& i)
{
    auto p = place_drawing(drawings, set, index, tg::pos3f(i.at[0], i.at[1], 0), tg::vec3f(i.x_axis[0], i.x_axis[1], 0),
                           tg::vec3f(i.y_axis[0], i.y_axis[1], 0), i.scale, i.tint, i.from);

    // How far the placed drawing reaches right of and below `at`: the largest offset any corner of its bounds lands at.
    auto const b = drawings.bounds(set, index);
    auto reach = tg::vec2f(0, 0);
    for (auto const x : {b.min[0], b.max[0]})
        for (auto const y : {b.min[1], b.max[1]})
        {
            auto const o = p.x_axis * x + p.y_axis * y;
            reach = tg::vec2f(cc::max(reach[0], o[0]), cc::max(reach[1], o[1]));
        }
    p.reach = reach;
    return p;
}

[[nodiscard]] drawing_placement place_3d(drawing_manager& drawings, drawing_set_id set, u32 index, instance_3d const& i)
{
    return place_drawing(drawings, set, index, i.at, i.x_axis, i.y_axis, i.scale, i.tint, sv::corner::top_left);
}

[[nodiscard]] decal_placement place_decal(drawing_manager& drawings, drawing_set_id set, u32 index, sv::decal const& d)
{
    CC_ASSERT(d.depth > 0.0f, "a decal's projection must reach some depth");
    auto const normal = tg::dual(cross(d.y_axis, d.x_axis));
    CC_ASSERT(tg::dot(normal, normal) > 0.0f, "a decal's axes must span a plane, which they do not by default");
    return {.first_record = drawings.first_record(set, index),
            .record_count = drawings.record_count(set, index),
            .bounds = drawings.bounds(set, index),
            .at = d.at,
            .x_axis = d.x_axis * d.scale,
            .y_axis = d.y_axis * d.scale,
            .depth = d.depth,
            .tint = sr::pack_rgba8(d.tint)};
}


} // namespace

// ---- mesh_ref / light_ref --------------------------------------------------------------------------------

scene_item& mesh_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer].items[_item];
}

void mesh_ref::transform(tg::affine_transform3f const& t)
{
    target().transform = t;
}

scene_item& quadric_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer].items[_item];
}

void quadric_ref::transform(tg::affine_transform3f const& t)
{
    if (_item == no_item)
        return; // an empty batch was never placed, so there is nothing to move

    target().transform = t;
}

scene_light& light_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer].lights[_light];
}

light_id light_ref::id() const
{
    return target().id;
}

void light_ref::light(sv::light const& l)
{
    auto const problem = light_problem(l);
    CC_ASSERTS(problem.empty(), problem);
    target().light = l;
}

light_ref& light_ref::candela(f32 value)
{
    target().light.candela(value);
    return *this;
}

light_ref& light_ref::lux(f32 value)
{
    target().light.lux(value);
    return *this;
}

light_ref& light_ref::nits(f32 value)
{
    target().light.nits(value);
    return *this;
}

light_ref& light_ref::lumens(f32 value)
{
    target().light.lumens(value);
    return *this;
}

light_ref& light_ref::color(tg::vec3f c)
{
    target().light.color(c);
    return *this;
}

light_ref& light_ref::exposure(f32 stops)
{
    target().light.exposure(stops);
    return *this;
}

light_ref& light_ref::face(light_face f)
{
    target().light.face(f);
    return *this;
}

light_ref& light_ref::cone(tg::angle_f inner_half_angle, tg::angle_f outer_half_angle)
{
    target().light.cone(inner_half_angle, outer_half_angle);
    return *this;
}

light_ref& light_ref::spread(tg::angle_f half_angle)
{
    target().light.spread(half_angle);
    return *this;
}

light_ref& light_ref::visible_to_camera(bool visible)
{
    target().light.visible_to_camera(visible);
    return *this;
}

light_ref& light_ref::casts_shadows(bool casts)
{
    target().light.casts_shadows(casts);
    return *this;
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

quadric_ref scene_ref::add_quadrics(sv::quadric_set const& set)
{
    // An empty batch draws nothing, so it is filtered out here rather than asserted on: a caller building a set from a
    // loop that happened to produce no primitives has not made a mistake, and `frame::_flush_immediate_quadrics`
    // already skips one for the same reason.
    // The lower-level asserts stay as contracts — see `gpu_resource_manager::create_quadric_set`.
    if (set.is_empty())
        return quadric_ref(_frame, _view, _layer, quadric_ref::no_item);

    // Content-keyed like `add_mesh`, so a caller re-adding an unchanged batch every frame pays lookups.
    auto& items = target().items;
    items.push_back(_frame->resources().acquire_scene_item(set));
    return quadric_ref(_frame, _view, _layer, u32(items.size() - 1));
}

quadric_ref scene_ref::add_quadrics(sv::resident_quadric_set const& set)
{
    if (set.primitive_count == 0)
        return quadric_ref(_frame, _view, _layer, quadric_ref::no_item);

    auto& items = target().items;
    items.push_back(_frame->resources().acquire_scene_item(set));
    return quadric_ref(_frame, _view, _layer, u32(items.size() - 1));
}

void scene_ref::add_sphere(tg::sphere3f const& sphere, material_id material)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_sphere(sphere);
}

void scene_ref::add_line(tg::segment3f const& segment, line_style const& style, material_id material)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_line(segment, style);
}

void scene_ref::add_line(tg::segment3f const& segment, float radius, material_id material)
{
    add_line(segment, line_style{.radius = radius}, material);
}

void scene_ref::add_cone(tg::segment3f const& base_to_apex, float base_radius, material_id material, bool capped)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_cone(base_to_apex, base_radius, capped);
}

void scene_ref::add_arrow(tg::segment3f const& segment, material_id material)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_arrow(segment);
}

void scene_ref::add_arrow(tg::segment3f const& segment, float shaft_radius, material_id material)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_arrow(segment, shaft_radius);
}

void scene_ref::add_arrow(tg::segment3f const& segment, arrow_style const& style, material_id material)
{
    _frame->_immediate_batch_for(_view, _layer, material).add_arrow(segment, style);
}

void scene_ref::add_drawing(drawing_set const& set, drawing_id id, instance_3d const& instance)
{
    CC_ASSERT(u32(id) < u32(set.size()), "a drawing_id names a drawing of the set that minted it");
    auto& drawings = _frame->resources().drawings;
    target().drawings.push_back(place_3d(drawings, drawings.acquire(set), u32(id), instance));
}

void scene_ref::add_drawing(drawing const& d, instance_3d const& instance)
{
    auto& drawings = _frame->resources().drawings;
    target().drawings.push_back(place_3d(drawings, drawings.acquire(d), 0, instance));
}

void scene_ref::add_decal(drawing_set const& set, drawing_id id, sv::decal const& decal)
{
    CC_ASSERT(u32(id) < u32(set.size()), "a drawing_id names a drawing of the set that minted it");
    auto& drawings = _frame->resources().drawings;
    target().decals.push_back(place_decal(drawings, drawings.acquire_decal(set), u32(id), decal));
}

void scene_ref::add_decal(drawing const& d, sv::decal const& decal)
{
    auto& drawings = _frame->resources().drawings;
    target().decals.push_back(place_decal(drawings, drawings.acquire_decal(d), 0, decal));
}

void scene_ref::add_text(cc::string_view text, instance_3d const& instance, text_style const& style)
{
    impl::place_text(_frame->resources().drawings, target().drawings, text, style, instance.at,
                     instance.x_axis * instance.scale, instance.y_axis * instance.scale, instance.tint,
                     sv::corner::top_left);
}

void scene_ref::add_annotation(tg::pos3f anchor, cc::string_view text, annotation_style const& style)
{
    auto& drawings = _frame->resources().drawings;
    auto a = annotation_record{.anchor = anchor,
                               .side = style.side,
                               .offset = style.offset,
                               .margin = style.margin,
                               .occluded = style.occluded,
                               .leader = style.leader.shape,
                               .leader_width = style.leader.width,
                               .leader_color = sr::pack_rgba8(style.leader.color),
                               .dashes = style.leader.dashes,
                               .hidden_dashes = style.leader.hidden_dashes,
                               .marker_radius = style.marker_radius};

    // The text first, inset by the padding, since the box is sized to it.
    auto text_placements = cc::vector<drawing_placement>();
    auto const extent = impl::place_text(drawings, text_placements, text, style.text,
                                         tg::pos3f(style.padding[0], style.padding[1], 0), tg::vec3f(1, 0, 0),
                                         tg::vec3f(0, 1, 0), tg::vec4f(1, 1, 1, 1), sv::corner::top_left);
    a.box_size = tg::vec2f(extent[0] + 2.0f * style.padding[0], extent[1] + 2.0f * style.padding[1]);

    // One drawing per box size and look, so an unchanged label reuses its box from the frame before.
    auto box = drawing();
    auto const outer = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(a.box_size[0], a.box_size[1]));
    box.add_fill(path::rounded_rectangle(outer, style.corner_radius), {.color = style.fill});
    if (style.border_width > 0.0f && style.border[3] > 0.0f)
    {
        auto const inset = style.border_width * 0.5f;
        auto const inner = tg::aabb2f(outer.min + tg::vec2f(inset, inset), outer.max - tg::vec2f(inset, inset));
        box.add_stroke(path::rounded_rectangle(inner, cc::max(style.corner_radius - inset, 0.0f)),
                       {.color = style.border, .width = style.border_width});
    }
    a.content.push_back(place_2d(drawings, drawings.acquire(box), 0, {}));
    for (auto const& t : text_placements)
        a.content.push_back(t);

    auto const parts = drawings.annotation_parts(style.marker_radius, cc::max(style.leader.width, 1.0f));
    auto const tint = instance_2d{.tint = style.leader.color};
    a.disk = place_2d(drawings, parts, 0, tint);
    a.ring = place_2d(drawings, parts, 1, tint);
    a.segment = place_2d(drawings, parts, 2, tint);
    a.dot = place_2d(drawings, parts, 3, tint);
    target().annotations.push_back(cc::move(a));
}

// ---- canvas_ref ------------------------------------------------------------------------------------------

layer& canvas_ref::target() const
{
    return _frame->_views[u32(_view)].layers[_layer];
}

void canvas_ref::add_drawing(drawing_set const& set, drawing_id id, instance_2d const& instance)
{
    CC_ASSERT(u32(id) < u32(set.size()), "a drawing_id names a drawing of the set that minted it");
    auto& drawings = _frame->resources().drawings;
    target().drawings.push_back(place_2d(drawings, drawings.acquire(set), u32(id), instance));
}

void canvas_ref::add_drawing(drawing const& d, instance_2d const& instance)
{
    auto& drawings = _frame->resources().drawings;
    target().drawings.push_back(place_2d(drawings, drawings.acquire(d), 0, instance));
}

light_ref scene_ref::add_light(cc::string_view id, sv::light const& light)
{
    CC_ASSERT(_frame->_open, "cannot author a closed frame");

    auto const problem = light_problem(light);
    CC_ASSERTS(problem.empty(), problem);

    auto const lid = light_id::from_string(id, _frame->_id_seed);
    auto& lights = target().lights;

    // Linear, since a layer's lights number in the tens; a set per layer would cost more than the scan.
    for (auto const& existing : lights)
        CC_ASSERT(existing.id != lid,
                  "duplicate light id in one scene layer — two lights with one id would share whatever the renderer "
                  "keeps per light; wrap the body in frame::scoped_id(i), suffix the id with ##i, or give them "
                  "distinct names");

    lights.push_back({.id = lid, .light = light});
    return light_ref(_frame, _view, _layer, u32(lights.size() - 1));
}

light_ref scene_ref::add_point_light(cc::string_view id, tg::pos3f position)
{
    return add_light(id, sv::light::point(position));
}

light_ref scene_ref::add_spot_light(cc::string_view id,
                                    tg::pos3f position,
                                    tg::vec3f direction,
                                    tg::angle_f outer_half_angle,
                                    tg::angle_f inner_half_angle)
{
    return add_light(id, sv::light::spot(position, direction, outer_half_angle, inner_half_angle));
}

light_ref scene_ref::add_rect_light(cc::string_view id, tg::pos3f center, tg::vec3f half_extent_u, tg::vec3f half_extent_v)
{
    return add_light(id, sv::light::rect(center, half_extent_u, half_extent_v));
}

light_ref scene_ref::add_directional_light(cc::string_view id, tg::vec3f direction)
{
    return add_light(id, sv::light::directional(direction));
}

light_ref scene_ref::add_sun_light(cc::string_view id, tg::vec3f direction, tg::angle_f angular_diameter)
{
    return add_light(id, sv::light::sun(direction, angular_diameter));
}

void scene_ref::fallback_light(cc::optional<sv::light> const& light)
{
    if (light.has_value())
    {
        auto const problem = light_problem(light.value());
        CC_ASSERTS(problem.empty(), problem);
    }
    target().fallback_light = light;
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

void leaf_ref::title(bool v)
{
    target().title = v;
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

canvas_ref view_ref::add_canvas()
{
    auto& v = target();
    v.layers.push_back({.kind = layer_kind::canvas, .blend = layer_blend::over});
    return canvas_ref(_frame, _view, u32(v.layers.size() - 1));
}

void canvas_ref::add_text(cc::string_view text, instance_2d const& instance, text_style const& style)
{
    impl::place_text(
        _frame->resources().drawings, target().drawings, text, style, tg::pos3f(instance.at[0], instance.at[1], 0),
        tg::vec3f(instance.x_axis[0], instance.x_axis[1], 0) * instance.scale,
        tg::vec3f(instance.y_axis[0], instance.y_axis[1], 0) * instance.scale, instance.tint, instance.from);
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

void view_ref::camera_cut()
{
    _frame->state_of(_view).camera_cut_pending = true;
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
