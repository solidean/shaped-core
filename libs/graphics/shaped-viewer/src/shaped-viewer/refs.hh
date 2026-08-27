#pragma once

#include <clean-core/common/utility.hh> // cc::forward
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/box_style.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/view/view_data.hh>

#include <type_traits>

/// The authoring handles a caller actually touches.
///
/// Every one of them is a non-owning `{frame*, index}` pair: the frame owns the pools, and a handle only names a slot
/// in them.
/// So they copy freely, none of them is a scope guard, and none may outlive the frame that made it.
///
/// Nesting is expressed by *holding* a handle rather than by an open-container stack: `r.add_view("a")` puts a view in
/// `r` whatever else happened in between, which is what lets two layouts be filled in any order.

/// One mesh placed in a scene.
///
/// Handed back by `scene_ref::add_mesh` so the placement can still be changed after the fact — and so per-kind
/// modification has a type to grow on, rather than every scene call funnelling through one builder.
class sv::mesh_ref
{
public:
    mesh_ref(frame* f, view_index view, u32 layer, u32 item) : _frame(f), _view(view), _layer(layer), _item(item) {}

    /// Where this placement puts the mesh, overriding the `sv::mesh`'s own transform.
    void transform(tg::affine_transform3f const& t);

private:
    [[nodiscard]] scene_item& target() const;

    frame* _frame = nullptr;
    view_index _view = view_index(0);
    u32 _layer = 0;
    u32 _item = 0;
};

/// One light in a scene — the counterpart of `mesh_ref`, handed back by `scene_ref::add_light`.
class sv::light_ref
{
public:
    light_ref(frame* f, view_index view, u32 layer, u32 light) : _frame(f), _view(view), _layer(layer), _light(light) {}

    /// Replaces the whole light.
    void light(area_light const& l);

private:
    [[nodiscard]] area_light& target() const;

    frame* _frame = nullptr;
    view_index _view = view_index(0);
    u32 _layer = 0;
    u32 _light = 0;
};

/// A 3D scene layer of a view — where geometry and lights go.
class sv::scene_ref
{
public:
    scene_ref(frame* f, view_index view, u32 layer) : _frame(f), _view(view), _layer(layer) {}

    /// Places a mesh in the scene, at the transform the mesh carries.
    ///
    /// The geometry and the per-face materials are uploaded here, keyed by the content hashes the mesh already
    /// carries — so calling this every frame with an unchanged mesh uploads nothing and stays O(1).
    /// Per-face PBR is read from the `sv::pbr_attribute` attributes; a mesh carrying none is shaded with
    /// `pbr_material`'s defaults.
    mesh_ref add_mesh(sv::mesh const& mesh);

    /// Adds an area light.
    /// A scene with none is still lit: the trace falls back to one key light rather than rendering black.
    light_ref add_light(area_light const& light);

    /// The environment a missed ray sees.
    void background(sv::background const& bg);

    /// Sample counts and bounce limits for this layer's trace.
    void settings(render_settings const& s);

private:
    [[nodiscard]] layer& target() const;

    frame* _frame = nullptr;
    view_index _view = view_index(0);
    u32 _layer = 0;
};

/// One view: a texture, and everything that goes into it.
///
/// Its layout layer is reached lazily — `layout_rows()` opens one the first time it is asked for — while `add_scene()`
/// appends a scene layer every time it is called.
///
/// Declared before the layout handles that hand one back, so their formatted `add_view` overloads can be written
/// inline.
class sv::view_ref
{
public:
    view_ref(frame* f, view_index view) : _frame(f), _view(view) {}

    /// Appends a 3D scene layer to this view.
    ///
    /// A traced layer writes no alpha, so it is forced to `layer_blend::replace`: a second scene layer overwrites the
    /// first rather than compositing over it (see libs/graphics/shaped-viewer/docs/TODO.md).
    /// Until that lands, calling this twice in a frame is legal but only the last layer is visible.
    [[nodiscard]] scene_ref add_scene();

    /// Fills this view with a layout tree, created on first use.
    /// `rows` stacks its children top to bottom, `columns` side by side; the params overload pins one dimension and
    /// derives the other, so `{.cols = 3}` fills rows of three.
    [[nodiscard]] layout_ref layout_rows(box_style style = {});
    [[nodiscard]] layout_ref layout_columns(box_style style = {});
    [[nodiscard]] layout_ref layout_grid(int cols, int rows, box_style style = {});
    [[nodiscard]] layout_ref layout_grid(grid_params params, box_style style = {});
    [[nodiscard]] layout_ref layout_auto_grid(box_style style = {}, grid_params params = {});

    /// The camera this view is drawn from, this frame.
    /// Setting it every frame means the caller owns it: the built-in controller leaves this view alone.
    void camera(sv::camera const& cam);

    /// The camera this view starts at, applied only the first time this id is seen.
    /// After that whatever the user orbited it to wins.
    void initial_camera(sv::camera const& cam);
    void initial_orbit(orbit_state const& o);

    /// Pins this view to a fixed pixel resolution instead of taking the rect it lands in.
    void resolution(tg::vec2i r);

    /// How often this view re-renders, as a fraction of the loop's rate: 1 every frame, 0.5 every second frame.
    void refresh_rate(float rate);

    /// Offers this view for dragging: Ctrl + left-drag lifts it out of the layout and floats it over the window.
    ///
    /// Opt-in because the caller rebuilds the tree every frame, so the viewer may only restructure what it was told it may.
    /// Once lifted, the view is placed by a fraction of the window rather than by the layout: its siblings tile as if
    /// it were absent, and it draws in front.
    /// Re-assert it every frame; dropping the call leaves the view where it was but stops it being draggable.
    void movable(bool v = true);

    /// What this view is called where a human reads it, which defaults to `display_name_of(id)` — the id up to its
    /// `##`.
    ///
    /// It is persistent, like the camera: set once and it survives every later frame that does not set it again.
    /// Setting an empty name restores the default rather than leaving the view nameless.
    /// Nothing draws it yet — sv has no text renderer — so this is what a title bar will read, not what one does.
    void display_name(cc::string_view name);

    template <class Arg0, class... Args>
    void display_name(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                      Arg0&& arg0,
                      Args&&... args)
    {
        display_name(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    [[nodiscard]] cc::string_view display_name() const;

    /// How many frames this view's first traced layer has blended into its accumulated image.
    ///
    /// It is 0 on any frame that restarts the accumulation, which is any frame changing what the image depends on —
    /// the camera, the scene, the resolution, or a shader reload.
    /// So a caller waiting for a converged image needs no "has anything changed" rule of its own; this counter already
    /// is one.
    /// What it does NOT see is post-load work a resource still owes, which changes a texture's contents rather than
    /// its id — `frame::pending_resource_work` is that half.
    [[nodiscard]] u32 accumulated_frames() const;

    [[nodiscard]] view_id id() const;
    [[nodiscard]] view_index index() const { return _view; }

private:
    [[nodiscard]] view_data& target() const;
    [[nodiscard]] layout_ref open_layout(box_style style, grid_params params);

    frame* _frame = nullptr;
    view_index _view = view_index(0);
};

/// A leaf of a layout — one or more views, how they combine, and how the result meets the leaf's rect.
class sv::leaf_ref
{
public:
    leaf_ref(frame* f, layout_node_id node) : _frame(f), _node(node) {}

    /// Adds a view to this leaf.
    /// More than one only makes sense under a post-process that combines them.
    ///
    /// `id` may be formatted (`add_view("pane {}", i)`) and may carry an ImGui-style `##` suffix, which separates two
    /// views sharing a display name.
    [[nodiscard]] view_ref add_view(cc::string_view id);

    template <class Arg0, class... Args>
    [[nodiscard]] view_ref add_view(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                                    Arg0&& arg0,
                                    Args&&... args)
    {
        return add_view(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    /// Combines this leaf's views into one image.
    void post_process(sv::post_process const& p);

    void fit(fit_mode m);
    void sampler(sampler_mode m);

    /// Whether the key-bound zoom may magnify this leaf.
    void allow_zoom(bool v = true);

private:
    [[nodiscard]] layout_leaf& target() const;

    frame* _frame = nullptr;
    layout_node_id _node = layout_node_id(0);
};

/// An open layout container — what `layout_rows()` and friends hand back.
///
/// Children are added through it, so it is the container: there is no "current" one, and nothing has to be closed.
class sv::layout_ref
{
public:
    layout_ref(frame* f, layout_node_id node) : _frame(f), _node(node) {}

    /// Adds a leaf holding one view, and returns that view.
    /// This is the common case — `r.add_view("main").add_scene()` is a whole pane.
    ///
    /// `id` may be formatted and may carry a `##` suffix, exactly as in `leaf_ref::add_view`.
    [[nodiscard]] view_ref add_view(cc::string_view id);

    template <class Arg0, class... Args>
    [[nodiscard]] view_ref add_view(cc::format_string<std::type_identity_t<Arg0>, std::type_identity_t<Args>...> fmt,
                                    Arg0&& arg0,
                                    Args&&... args)
    {
        return add_view(cc::format(fmt, cc::forward<Arg0>(arg0), cc::forward<Args>(args)...));
    }

    /// Adds an empty leaf, for a caller that wants several views in one cell or a post-process over them.
    [[nodiscard]] leaf_ref leaf();

    /// Nests another container inside this one.
    ///
    /// All four are the same container with a different `grid_params`: `rows` pins one column, `columns` one row,
    /// `grid` both counts, and `auto_grid` neither.
    /// The params overload is the one that pins a single dimension — `{.cols = 3}` fills rows of three.
    [[nodiscard]] layout_ref rows(box_style style = {});
    [[nodiscard]] layout_ref columns(box_style style = {});
    [[nodiscard]] layout_ref grid(int cols, int rows, box_style style = {});
    [[nodiscard]] layout_ref grid(grid_params params, box_style style = {});
    [[nodiscard]] layout_ref auto_grid(box_style style = {}, grid_params params = {});

    /// Nests a container placed by fraction of this one, and taken out of its flow.
    /// Siblings tile as if it were absent, and it draws in front — an inset, an overlay, a dragged-out pane.
    [[nodiscard]] layout_ref relative(relative_placement placement, box_style style = {});

    /// This container's margin, border, padding and the spacing between its children.
    void style(box_style const& s);

    [[nodiscard]] layout_node_id node() const { return _node; }

private:
    frame* _frame = nullptr;
    layout_node_id _node = layout_node_id(0);
};

/// Everything a view offers, forwarded to whatever `Derived` calls its default view.
///
/// This is what lets `f.add_scene().add_mesh(m)` mean "the default window's default view's 3D scene" without a caller
/// spelling the chain out — and what keeps that shorthand honest, since it resolves to the very same handle.
template <class Derived>
struct sv::view_api
{
    [[nodiscard]] scene_ref add_scene() { return self().default_view().add_scene(); }

    [[nodiscard]] layout_ref layout_rows(box_style style = {}) { return self().default_view().layout_rows(style); }
    [[nodiscard]] layout_ref layout_columns(box_style style = {})
    {
        return self().default_view().layout_columns(style);
    }
    [[nodiscard]] layout_ref layout_grid(int cols, int rows, box_style style = {})
    {
        return self().default_view().layout_grid(cols, rows, style);
    }
    [[nodiscard]] layout_ref layout_grid(grid_params params, box_style style = {})
    {
        return self().default_view().layout_grid(params, style);
    }
    [[nodiscard]] layout_ref layout_auto_grid(box_style style = {}, grid_params params = {})
    {
        return self().default_view().layout_auto_grid(style, params);
    }

    void camera(sv::camera const& cam) { self().default_view().camera(cam); }
    void initial_camera(sv::camera const& cam) { self().default_view().initial_camera(cam); }
    void initial_orbit(orbit_state const& o) { self().default_view().initial_orbit(o); }
    void refresh_rate(float rate) { self().default_view().refresh_rate(rate); }

private:
    [[nodiscard]] Derived& self() { return static_cast<Derived&>(*this); }
};

/// Everything a window offers, on top of its default view's surface.
template <class Derived>
struct sv::window_api : view_api<Derived>
{
    /// This window's default view — the one that fills it.
    [[nodiscard]] view_ref view() { return static_cast<Derived&>(*this).default_view(); }
};

/// One window of a frame.
///
/// A window's default view *is* its root view: the texture the window presents.
/// So `f.window().view().layout_rows()` fills the window with a layout, and `f.window().add_scene()` fills it with one
/// 3D scene instead.
class sv::window_ref : public window_api<window_ref>
{
public:
    window_ref(frame* f, u32 window) : _frame(f), _window(window) {}

    /// The root view this window presents.
    [[nodiscard]] view_ref default_view() const;

private:
    frame* _frame = nullptr;
    u32 _window = 0;
};
