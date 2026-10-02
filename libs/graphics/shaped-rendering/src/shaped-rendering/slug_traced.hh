#pragma once

#include <clean-core/container/array.hh>
#include <clean-core/container/span.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/pos.hh>

/// Shapes as ray-traced geometry: each instance a quad over its em box, two non-opaque triangles an inline trace's any-hit
/// cuts down to the shape — module `slug`'s `decide`, which reads `slug.tables` and `slug.shapes`:
///
///     let h = scene.world.trace(r, c => slug.decide(c))
///
///     auto const records = sr::upload_slug_records(*cmd, atlas, instances);    // every instance, once
///     auto const blas = sr::build_slug_blas(*cmd, instances.subspan({.offset = first, .size = count}));
///     sg::tlas_instance const label = {.blas = blas, .transform = ..., .instance_id = u32(first),
///                                      .cull_mode = sg::instance_cull_mode::none};
///
/// **A TLAS instance of a run's BLAS carries the index of the run's first record as its instance_id**, which is how the
/// decision finds a candidate's record, so a trace's slug instances all index one record buffer.
/// The quads lie on the object's xy plane where the instances place them, as sr::slug_routine draws them.
/// Their winding follows each shape's axes, so the TLAS instance culls nothing.

namespace sr
{
/// Uploads `instances` as the record buffer `slug.shapes` binds, on `cmd`, and prepares `atlas`, which every one names a shape of.
/// The buffer is persistent: it lives as long as the caller holds it, across frames.
[[nodiscard]] sg::buffer<sgl_modules::slug::shape_instance> upload_slug_records(sg::command_list& cmd,
                                                                                slug_atlas& atlas,
                                                                                cc::span<slug_instance const> instances);

/// Builds the quads of a run of instances into a BLAS of one non-opaque geometry, quad i being triangles 2i and 2i + 1.
/// `run` must not be empty, and `cmd`'s device must support ray queries.
/// `hit_record_stride` is `build_blas`'s, which only a ray-tracing pipeline reads.
[[nodiscard]] sg::blas_handle build_slug_blas(sg::command_list& cmd,
                                              cc::span<slug_instance const> run,
                                              int hit_record_stride = 1);

/// The quads' vertices in object space, six per instance, in the order module `slug`'s `quad_em` reads them back.
[[nodiscard]] cc::array<tg::pos3f> slug_quad_vertices(cc::span<slug_instance const> run);
} // namespace sr
