#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <sgl_modules/tracer.hh>                                // the instance, light and emitter records
#include <shaped-graphics/raytracing/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// One view resolved to what the path tracer binds: a TLAS instance and an `shaders::tracer::instance_record` per item, over the set of
/// permutations those items shade with.
///
/// Two index relations, which together are the shape of the trace:
/// `instances[i]` and `records[i]` describe the same item, and `instances[i].hit_group_offset` indexes `hit_groups`.
/// A view resolved for a raster frame names no BLAS: its instances carry the placement and the permutation alone.
struct sv::impl::resolved_view
{
    cc::vector<sg::tlas_instance> instances;
    cc::vector<shaders::tracer::instance_record> records;

    /// the parameter block each item shades with, parallel to `records` — its CONTENT identity, for the trace hash
    cc::vector<instance_id> parameter_blocks;

    /// Per instance, the geometry's content key when it is a frame mesh, and zero when a manager holds it.
    /// A frame mesh's BLAS and buffers are new every frame, so the trace hash reads this in their place.
    cc::vector<cc::hash128> frame_contents;

    /// Per instance, its geometry's content key whoever holds it, and zero for a placeholder box.
    /// What the seed reads in place of a BLAS address, since it is equal on any machine.
    cc::vector<cc::hash128> contents;

    /// Per instance, a frame mesh's own index buffer, which a raster frame binds to draw its row; null where the row's indices are in the mesh manager's pool.
    cc::vector<sg::buffer<u32>> own_indices;
    cc::vector<material_permutation const*> hit_groups;

    /// the shader key of `hit_groups[i]`, kept for the trace hash rather than for the pipeline
    cc::vector<cc::hash128> permutations;

    /// How many triangles `instances[i]`'s geometry holds — a placeholder's cube included — and 0 for a landed quadric batch.
    /// What a raster frame draws per row; no trace reads it.
    cc::vector<u32> triangle_counts;
    /// How many primitives each row's landed quadric batch holds, which a raster frame draws a box for each of; 0 for any other row.
    cc::vector<u32> primitive_counts;

    /// The box of `instances[i]`'s geometry in its own space, which a raster frame culls the row by; none where nothing declared one.
    cc::vector<cc::optional<tg::aabb3f>> bounds;

    /// One per instance that lights the scene this frame, in item order: its light record, and what the sampler reads beyond it.
    /// `emitters[k].instance` is the row of `records` it describes; `link_emitters` writes that row's light index once the table is grouped.
    cc::vector<shaders::tracer::light_record> emitter_lights;
    cc::vector<shaders::tracer::emitter_record> emitters;

    /// How many traced instances are emitters shown to emit, whether or not their area CDF is built yet.
    /// What turns the fallback light off: a layer lit by its emitters is lit, even on the frames before they can be sampled.
    /// Shown means an estimate that is neither `unseen` nor resting on a factor not summarized yet, and a permutation that has not failed.
    /// One still compiling counts, so the fallback does not switch on for every cold compile.
    /// A raster frame counts none, as it draws no emitter's light, and neither does a manager whose CDF kernels failed.
    isize emissive_items = 0;

    /// Whether some emissive instance lit nothing this frame because its area CDF is not built yet, or its permutation is still compiling.
    /// Such a frame is an image of a darker scene, so it restarts as a frame drawn through a stand-in does.
    bool emitters_pending = false;

    /// Whether any item here is a quadric batch at all.
    ///
    /// It decides whether the trace is handed the PROCEDURAL stand-in, and it has to be this broad.
    /// Acquiring the stand-in starts its compile, and `pathtrace_routine::_variant_for` declines the WHOLE trace —
    /// meshes included — while a quadric permutation is unready and no procedural stand-in is ready to take its place.
    /// So deferring the acquire until some batch is actually caught uncompiled means the view blanks for exactly as
    /// long as the stand-in then takes to build, which is the outage the stand-in exists to prevent.
    /// Acquiring it as soon as a batch is placed gives it the whole life of the view to get ready in.
    ///
    /// What still holds, and is the reason this is not simply unconditional, is that a triangle-only scene never
    /// acquires it and never compiles a hit group it could not select.
    bool has_quadrics = false;
};
