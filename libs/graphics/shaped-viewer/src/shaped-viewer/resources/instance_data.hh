#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/shader_generator.hh> // material_slot_kind, which a slot carries

/// One scene item as a closest-hit reads it, indexed by `InstanceID()` — mirrors `sv_instance` in shaders/material_runtime.hlsli.
///
/// Everything a hit needs to shade is reached from here: the material's parameter block, and the geometry the hit is on.
/// Nothing the path tracer binds is per-mesh any more except the table itself, so a view may hold any number of meshes with any
/// number of materials between them.
///
/// Every field but `is_indexed` is a bindless index acquired **this epoch**, by the same call that fills this record.
/// The table is rebuilt each frame and so are the indices in it, which is what keeps `bound_resources`'s access declaration
/// complete: nothing a hit reads reached the GPU without going through an acquire.
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

/// One resolved parameter slot, as much of it as outlives the resolution it came from.
///
/// A parameter block is rebuilt every epoch, because every index in it is that epoch's, so what a `scene_item` carries between
/// frames is this rather than bytes.
/// Exactly one payload is live, and `kind` says which.
struct sv::instance_slot
{
    material_slot_kind kind = material_slot_kind::constant;

    /// byte offset into the block, and how much of it this slot takes — both the layout's
    i32 offset = 0;
    i32 size_bytes = 0;

    /// `kind == constant`: the value itself, copied out of the resolution it borrowed from
    cc::vector<byte> constant;

    /// `kind == attribute_descriptor`: the uploaded attribute, and the stride its elements are packed at
    attribute_id attribute = attribute_id::invalid;
    u32 element_stride = 0;

    /// `kind == texture_index`: the uploaded texture the block names
    texture_id texture = texture_id::invalid;
};

/// One instance's material parameter block, resolved down to ids — everything about it that survives an epoch.
///
/// `shader_key` says which generated permutation reads it, which is what a hit-group assignment is keyed on.
/// Minted by `gpu_resource_manager::acquire_instance` and content-cached on the resolved material's `parameter_key`, so two
/// meshes drawn identically share one.
struct sv::instance_record
{
    cc::hash128 shader_key;

    /// how big the block is, which is `material_parameter_layout::size_bytes`
    i32 size_bytes = 0;

    cc::vector<instance_slot> slots;
};
