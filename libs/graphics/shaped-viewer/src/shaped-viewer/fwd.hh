#pragma once

#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>

#include <memory>

/// Aggregate forward declarations for shaped-viewer.
///
/// This is also sv's one door to `<memory>`: sg hands out command lists as `std::unique_ptr<sg::command_list>`, so code holding one includes this header rather than `<memory>` itself.

namespace sv
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sv, not leaked globally.
using namespace cc::primitive_defines;

// vocabulary
// GPU resource managers (see resources/resource_managers.hh)
struct resource_budget;
struct manager_config;
struct mesh_record;
struct material_record;
struct scene_resources_config;

struct gpu_boolean;
struct view_id;
struct camera;
struct camera_basis;
struct perspective_projection;
struct camera_gpu;
struct orbit_state;
struct camera_controller_config;
class orbit_camera_controller;
struct fps_state;
struct fps_camera_controller_config;
class fps_camera_controller;
struct pbr_material;
struct pbr_material_gpu;
struct area_light;
struct area_light_gpu;
struct background;
struct background_gpu;
struct render_settings;
struct scene_item;
enum class scene_item_kind : u8; // which arm of a scene_item is live (scene_item.hh)
struct triangle_geometry;
enum class scalar_type : u8;
struct attribute_format;
enum class attribute_frequency : u8;
enum class mesh_flag; // per-mesh rendering opt-ins (mesh_flags.hh)
struct mesh_attribute;
struct parameter_value;
struct mesh_parameter;
struct mesh_texture;
struct mesh;

// the per-frame description
enum class layer_kind : u8;
enum class layer_blend : u8;
struct layer;
struct temporal_input;
struct refresh_policy;
struct view_data;
struct viewer_definition;

/// A view's slot in the frame's view pool — `viewer_definition::views`, which a layout leaf and the plan both name it by.
///
/// Distinct from `view_id`, the identity a view keeps across frames: this only says where the view sits in *this* frame.
/// Strongly typed for the reason `layout_node_id` is — a definition and a plan are full of other u32 indices (node,
/// target, trace, hit region) that a raw view index silently converts into.
enum class view_index : u32;
struct frame_constants_gpu;

// resource upload data + ids + managers
struct triangle_data;
struct indexed_triangle_data;
struct material_data;
enum class mesh_id : u32;
enum class material_set_id : u32;
enum class material_id : u32;
enum class tlas_id : u32;
enum class texture_id : u32;
enum class buffer_id : u32;
class mesh_manager;
class material_manager;
class texture_manager;
// The class-key must match the definition: the Microsoft ABI mangles struct and class differently, so a
// mismatch here links a TU that only saw this declaration against a symbol nobody defines.
class scene_resources;

// render plan
enum class draw_kind : u8;
enum class draw_source_kind : u8;
enum class diagnostic_reason : u8;
struct draw_source;
struct layout_draw;
struct plan_target;
struct plan_trace;
struct hit_region;
struct plan_diagnostic;
struct view_history_entry;
struct view_history;
struct render_plan;
enum class window_id : u32;
struct plan_textures;
class layout_routine;

struct plan_resources;

namespace impl
{
struct layout_pipeline_key;
struct accumulation_slot;
} // namespace impl

// rendering
struct trace_desc;
class pbr_raytrace_routine;
struct pt_frame_constants_gpu;
struct pt_trace_desc;
class pathtrace_routine;
class view_renderer;
class viewer_renderer;

// immediate-mode viewer
struct viewer_config;
class viewer;
class frame;
class frame_scope;
class id_scope;
class frame_range;
class frame_iterator;
struct frame_sentinel;

// authoring handles (see refs.hh) — all non-owning {frame*, index} pairs
class mesh_ref;
class light_ref;
class scene_ref;
class leaf_ref;
class layout_ref;
class view_ref;
class window_ref;

template <class Derived>
struct view_api;
template <class Derived>
struct window_api;

/// The name of the window a frame creates when a caller never asks for one by name.
inline constexpr char const* default_window_name = "window";

// layout
struct box_insets;
struct box_style;
struct grid_params;
struct grid_dims;
enum class layout_kind : u8;
enum class fit_mode : u8;
enum class sampler_mode : u8;
struct relative_placement;
struct layout_leaf;
struct layout_node;
struct layout_tree;
struct resolved_item;
struct layout_solution;

/// A node's slot in a `layout_tree`, and the "no node" sentinel.
///
/// Here rather than in layout_tree.hh because a layer references a node without needing the tree's shape.
/// Strongly typed because the plan is full of other u32 indices — target, trace, hit region — that a raw node index
/// silently converts into.
enum class layout_node_id : u32;
inline constexpr layout_node_id invalid_node = layout_node_id(-1);

// post-processing
enum class post_process_kind : u8;
struct post_process;
} // namespace sv
