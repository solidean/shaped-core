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

/// One instance's material parameter block: the slots it is built from, and where in the parameter pool it lives.
///
/// Minted by `gpu_resource_manager::acquire_instance` and content-cached on the resolved material's `parameter_key`, so two
/// meshes drawn identically share one.
/// Which generated permutation reads the block is `scene_item::shader_key`'s to say, and is deliberately not repeated here:
/// two materials differing only in their sampler share a `parameter_key` while splitting the permutation, so a copy stored
/// beside the shared block could name either of them.
///
/// **The range is persistent and the bytes in it are not.**
/// Every index a block holds is the epoch's that wrote it, so `describe_instance` rebuilds the bytes each epoch — but into the
/// same range of the manager's one parameter pool, and it uploads only when they differ from `uploaded`.
/// A stable working set therefore writes no descriptor, which is what keeps the staging group clean and its snapshot cached.
struct sv::instance_record
{
    /// how big the block is, which is `material_parameter_layout::size_bytes`
    i32 size_bytes = 0;

    cc::vector<instance_slot> slots;

    /// The block's byte offset in the manager's parameter pool, allocated on the first `describe_instance`; -1 before.
    /// At least 4 bytes are reserved even for an empty block, so every record names a range.
    isize param_offset = -1;

    /// The pool generation `uploaded` went into: a pool replaced by a larger one holds nothing until each block uploads again.
    u64 param_generation = 0;

    /// what was last uploaded into the pool, so an unchanged block costs a compare rather than a copy
    cc::vector<byte> uploaded;

    /// The manager's epoch stamp the block was last built in.
    /// Within an epoch a rebuild acquires the same indices again, so the items sharing this record, and every later recording
    /// of the epoch, reuse that build — even one a texture landing in between would have changed, which waits for the next epoch.
    u64 built_in = 0;

    /// The manager's epoch stamp in which every attribute the block names was last seen resident.
    /// Residency only ever drops at an epoch's start, when eviction runs, so a check that passed holds for the rest of the epoch.
    u64 resident_in = 0;
};
