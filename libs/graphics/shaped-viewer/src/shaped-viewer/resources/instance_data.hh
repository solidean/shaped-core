#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/shader_generator.hh> // material_slot_kind, which a slot carries
#include <typed-geometry/linalg/vec.hh>

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

    /// `kind == texture_index`: the texel a 1x1 placeholder is filled with while that texture is still arriving.
    ///
    /// Already inverted through the sample's transform and scattered through its swizzle, so what the shader computes
    /// from it is the material's own factor — a base color map that has not landed reads as the base color rather
    /// than as black.
    /// Channels the swizzle never reads are 1, since nothing samples them through this slot.
    tg::vec4f placeholder_texel = tg::vec4f(1, 1, 1, 1);
};

/// One instance's material parameter block: the slots it is built from, and the buffer it is built into.
///
/// Minted by `gpu_resource_manager::acquire_instance` and content-cached on the resolved material's `parameter_key`, so two
/// meshes drawn identically share one.
/// Which generated permutation reads the block is `scene_item::shader_key`'s to say, and is deliberately not repeated here:
/// two materials differing only in their sampler share a `parameter_key` while splitting the permutation, so a copy stored
/// beside the shared block could name either of them.
///
/// **The buffer is persistent and the bytes in it are not.**
/// Every index a block holds is the epoch's that wrote it, so `describe_instance` rebuilds the bytes each epoch — but into the
/// same buffer, and it uploads only when they actually differ from `uploaded`.
/// A stable working set therefore writes no descriptor, which is what keeps the staging group clean and its snapshot cached;
/// a fresh transient buffer per frame would re-mint the whole bindless table on every trace.
struct sv::instance_record
{
    /// how big the block is, which is `material_parameter_layout::size_bytes`
    i32 size_bytes = 0;

    cc::vector<instance_slot> slots;

    /// Where the block lives, created on the first `describe_instance`.
    /// At least 4 bytes even for an empty block: a zero-sized buffer has no descriptor to acquire.
    sg::buffer<byte> parameters;

    /// what was last uploaded into `parameters`, so an unchanged block costs a compare rather than a copy
    cc::vector<byte> uploaded;
};
