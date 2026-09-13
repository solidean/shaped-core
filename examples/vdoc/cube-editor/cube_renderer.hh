#pragma once

#include "cube_components.hh"

#include <clean-core/container/vector.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <typed-geometry/linalg/mat.hh>
#include <versioned-document/document.hh>

/// Draws every cube in a document as one instanced draw.
///
/// sr has no mesh or debug-geometry routine yet — its routines are blit and imgui — so this example declares its own
/// shader package and its own routine, which is the ordinary way to draw anything custom today.

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

/// The cube pass as a render routine, parametrized on the target's pixel format.
///
/// A raster pipeline bakes its color-target format in, so "draw the cubes" is a schema rather than one unit of work:
/// the registry holds one instance per format, each owning the one pipeline it needs.
/// `init` compiles the shaders and builds that pipeline; nothing is built on the frame path.
class cube_routine : public sg::render_routine<cube_routine, sg::pixel_format>
{
public:
    /// Rebuilds the instance buffer from `doc` and draws it into the open scope.
    /// `selected` is highlighted; pass a default-constructed id for none.
    ///
    /// Declines while the shaders are still compiling, and after a compile that failed — the target format is only
    /// known once the caller's scope is open, so the acquire happens here rather than through a token.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     vdoc::document const& doc,
                                                     tg::mat4f const& view_projection,
                                                     vdoc::entity_id selected);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// The one pipeline this instance is for — its format is params().
    /// Built during init and only polled by execute, so nothing on the frame path ever waits for a compile.
    sg::async_raster_pipeline _pipeline;

    cc::vector<cube_instance> _instances; // kept across frames so the per-frame rebuild reuses the allocation
};

/// Every live cube in `doc`, in entity order.
/// A two-component join: vdoc walks the two sorted arrays together, so an entity with only one of them is skipped.
[[nodiscard]] cc::vector<cube_instance> collect_instances(vdoc::document const& doc, vdoc::entity_id selected);
} // namespace cube_editor
