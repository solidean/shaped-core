#pragma once

#include "cube_components.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/linalg/mat.hh>
#include <versioned-document/document.hh>

/// Draws every cube in a document as one instanced draw.
///
/// sr has no mesh or debug-geometry routine yet — its routines are blit and imgui — so this example declares its own
/// shader package and its own pipeline, which is the ordinary way to draw anything custom today.

namespace cube_editor
{
/// One cube's per-instance data, matching slot 1 of shaders/cube.hlsl.
struct cube_instance
{
    tg::pos3f center;
    tg::vec3f half_extent;
    tg::vec3f color;
    float highlight = 0.0f;
};

class renderer
{
public:
    /// Compiles the shaders and builds the layout; the pipelines themselves are built lazily, per target format.
    /// Fails only when the shaders did not compile, which is worth reporting rather than drawing nothing silently.
    ///
    /// Heap-held rather than returned by value because a pipeline cache is pinned: it guards its map with a mutex,
    /// so the renderer around it cannot be moved either.
    [[nodiscard]] static cc::result<cc::unique_ptr<renderer>> create(sg::context& ctx, slib::shader_library& lib);

    /// Rebuilds the instance buffer from `doc` and draws it into the open scope.
    /// `selected` is highlighted; pass a default-constructed id for none.
    void draw(sg::rendering_scope& scope, vdoc::document const& doc, tg::mat4f const& view_projection, vdoc::entity_id selected);

private:
    sr::keyed_pipeline_cache<sg::pixel_format> _pipelines;
    cc::vector<cube_instance> _instances; // kept across frames so the per-frame rebuild reuses the allocation
};

/// Every live cube in `doc`, in entity order.
/// A two-component join: vdoc walks the two sorted arrays together, so an entity with only one of them is skipped.
[[nodiscard]] cc::vector<cube_instance> collect_instances(vdoc::document const& doc, vdoc::entity_id selected);
} // namespace cube_editor
