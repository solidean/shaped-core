#pragma once

/// Full umbrella include for shaped-viewer's public surface.
/// Prefer the specific header you need.

#include <shaped-viewer/fwd.hh>

// immediate-mode viewer
#include <shaped-viewer/context.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/interactive.hh>
#include <shaped-viewer/refs.hh>
#include <shaped-viewer/viewer.hh>

// what a caller puts into a view
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/mesh_flags.hh>
#include <shaped-viewer/scene/mesh_texture.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <shaped-viewer/scene/scene_item.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>

// what a view is, and how it is framed
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <shaped-viewer/view/layer.hh>
#include <shaped-viewer/view/post_process.hh>
#include <shaped-viewer/view/render_settings.hh>
#include <shaped-viewer/view/view_data.hh>
#include <shaped-viewer/view/view_id.hh>
#include <shaped-viewer/view/view_store.hh>
#include <shaped-viewer/view/viewer_definition.hh>

// layout
#include <shaped-viewer/layout/box_style.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/layout/solvers.hh>

// resources
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/resource_managers.hh>

// rendering
#include <shaped-viewer/rendering/frame_constants.hh>
#include <shaped-viewer/rendering/layout_routine.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/raytrace_routine.hh>
#include <shaped-viewer/rendering/render_plan.hh>
#include <shaped-viewer/rendering/shaders.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/rendering/viewer_renderer.hh>
