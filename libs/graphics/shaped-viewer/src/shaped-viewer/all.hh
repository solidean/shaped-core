#pragma once

/// Full umbrella include for shaped-viewer's public surface.
/// Prefer the specific header you need.

#include <shaped-viewer/fwd.hh>

// vocabulary
#include <shaped-viewer/background.hh>
#include <shaped-viewer/camera.hh>
#include <shaped-viewer/gpu_types.hh>
#include <shaped-viewer/light.hh>
#include <shaped-viewer/mesh.hh>
#include <shaped-viewer/mesh_attribute.hh>
#include <shaped-viewer/mesh_flags.hh>
#include <shaped-viewer/mesh_parameter.hh>
#include <shaped-viewer/mesh_texture.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/render_settings.hh>
#include <shaped-viewer/scene_item.hh>
#include <shaped-viewer/triangle_geometry.hh>
#include <shaped-viewer/view.hh>
#include <shaped-viewer/view_id.hh>
#include <shaped-viewer/viewer_definition.hh>

// resources
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/resource_ids.hh>
#include <shaped-viewer/resources/resource_managers.hh>

// rendering
#include <shaped-viewer/rendering/frame_constants.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/raytrace_routine.hh>
#include <shaped-viewer/rendering/shaders.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
