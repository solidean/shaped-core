#pragma once

#include <clean-core/fwd.hh>
#include <shaped-rendering/fwd.hh>

/// Aggregate forward declarations for shaped-viewer.

namespace sv
{
// Vocabulary types (i32/u32/f32/isize/...) available bare inside sv, not leaked globally.
using namespace cc::primitive_defines;

// vocabulary
struct view_id;
struct camera;
struct perspective_projection;
struct camera_gpu;
struct pbr_material;
struct pbr_material_gpu;
struct area_light;
struct background;
struct background_gpu;
struct render_settings;
struct scene_item;
struct view;
struct viewer_definition;
struct frame_constants_gpu;

// resource ids + managers
enum class mesh_id : u32;
enum class material_set_id : u32;
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
