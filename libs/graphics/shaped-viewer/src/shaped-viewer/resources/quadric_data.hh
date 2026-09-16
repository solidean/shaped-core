#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/quadric.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// One quadric primitive as the intersection shader reads it, by `PrimitiveIndex()`.
///
/// Keep this and the HLSL `sv::quadric` in lockstep: it is a byte layout, not a description of one.
///
/// It is the CPU `sv::quadric_primitive` MINUS its box, because the two travel to the GPU in different buffers.
/// The boxes are the procedural BLAS's own build input and are never read by a shader — an intersection shader cannot reach the
/// acceleration structure's boxes anyway — so carrying them here would upload every one of them twice.
struct sv::quadric_gpu
{
    tg::pos3f origin;
    sv::quadric3 surface;
    sv::quadric3 clip;

    [[nodiscard]] static quadric_gpu of(quadric_primitive const& p)
    {
        return {.origin = p.origin, .surface = p.surface, .clip = p.clip};
    }
};

namespace sv
{
static_assert(sizeof(quadric_gpu) == 92, "quadric_gpu must match sv::quadric in the quadric shader runtime");
} // namespace sv

/// What a caller hands the quadric manager: the primitives, plus the content hash that identifies them.
///
/// **The span is BORROWED, and only has to outlive the acquire call.**
/// That is a deliberate difference from `sv::triangle_data` and its siblings, which pin their payload.
/// A quadric batch cannot be uploaded in the shape it is authored in — it splits into a primitive buffer and a box buffer — so
/// the acquire has to build new arrays anyway, and only on a cache miss.
/// Pinning the authored form up front would pin bytes nothing ever uploads, on the path taken every frame.
struct sv::quadric_data
{
    cc::span<quadric_primitive const> primitives;
    cc::hash128 hash;

    /// The extent of every primitive, in the set's own frame.
    ///
    /// NOT part of the hash: it is a summary of the same bytes, so two acquires of one batch that disagree about it are a caller
    /// error rather than two resources.
    /// The manager keeps it after the payload is gone, because a placeholder drawn while the batch is still streaming needs an
    /// extent to be drawn at.
    cc::optional<tg::aabb3f> bounds;

    /// The payload for an authored set, sharing its key rather than re-hashing.
    [[nodiscard]] static quadric_data of(quadric_set const& set)
    {
        return {.primitives = set.primitives(), .hash = set.hash(), .bounds = set.bounds()};
    }
};
