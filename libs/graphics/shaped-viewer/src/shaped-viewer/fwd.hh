#pragma once

#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>

/// Aggregate forward declarations for shaped-viewer.

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
struct perspective_projection;
struct camera_gpu;
struct pbr_material;
struct pbr_material_gpu;
struct area_light;
struct area_light_gpu;
struct background;
struct background_gpu;
struct render_settings;
struct scene_item;
enum class scene_item_kind : u8; // which arm of a scene_item is live (scene_item.hh)
enum class mesh_flag;            // per-mesh rendering opt-ins (mesh_flags.hh)
struct triangle_geometry;
enum class scalar_type : u8;
struct attribute_format;
enum class attribute_frequency : u8;
struct mesh_attribute;
struct parameter_value;
struct mesh_parameter;
struct mesh_texture;
struct mesh;
struct view;
struct viewer_definition;
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
struct scene_resources;

// rendering
struct trace_desc;
class pbr_raytrace_routine;
struct pt_frame_constants_gpu;
struct pt_trace_desc;
class pathtrace_routine;
class view_renderer;
} // namespace sv
