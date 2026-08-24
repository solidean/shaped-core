#pragma once

#include <shaped-viewer/fwd.hh>

/// One scene item as a closest-hit reads it, indexed by `InstanceID()` — mirrors `sv_instance` in shaders/material_runtime.hlsli.
///
/// This is the table that retires "one mesh per view".
/// Everything a hit needs to shade is reached from here: the material's parameter block, and the geometry the hit is on.
/// Nothing is a global binding any more except the table itself, so a view may hold any number of meshes with any number of
/// materials between them.
///
/// Every field but `is_indexed` is a **pinned** bindless index.
/// The table is rebuilt each frame while the resources it names are content-cached across frames, so an epoch-scoped index here
/// would name the wrong resource by the time the next frame read it.
///
/// `param_offset` exists so several parameter blocks may share one buffer.
/// They do not yet — one block is one buffer — but the shader already reads through it, so packing them later changes no contract.
struct sv::instance_gpu
{
    u32 param_buffer = 0; ///< index into gBindlessBuffers of the buffer holding this instance's material parameters
    u32 param_offset = 0; ///< byte offset of that block

    u32 vertices = 0; ///< index into gBindlessBuffers of the mesh's positions
    u32 indices = 0;  ///< the same for its indices; meaningless unless `is_indexed`

    /// Whether `indices` is live.
    /// Per instance rather than per frame, which is the point: geometry layout is a property of the mesh, and it rode in the frame constants only because the trace bound one mesh.
    u32 is_indexed = 0;

    u32 _padding[3] = {};
};

namespace sv
{
static_assert(sizeof(instance_gpu) == 32, "instance_gpu must match sv_instance in shaders/material_runtime.hlsli");
} // namespace sv
