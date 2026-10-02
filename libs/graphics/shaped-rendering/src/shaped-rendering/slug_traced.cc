#include <clean-core/common/asserts.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raytracing.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_traced.hh>

static_assert(sizeof(sr::slug_instance) == sizeof(sgl_modules::slug::shape_instance),
              "module slug's shape_instance is no longer sr::slug_instance's layout");

namespace sr
{
sg::buffer<sgl_modules::slug::shape_instance> upload_slug_records(sg::command_list& cmd,
                                                                  slug_atlas& atlas,
                                                                  cc::span<slug_instance const> instances)
{
    CC_ASSERT(!instances.empty(), "a trace with no shapes needs no records");
    atlas.prepare(cmd);

    auto records = cc::vector<sgl_modules::slug::shape_instance>();
    records.reserve(instances.size());
    for (auto const& i : instances)
        records.push_back({.em_to_object = i.em_to_object,
                           .origin = i.origin,
                           .em_bounds = i.em_bounds,
                           .banding = i.banding,
                           .glyph = tg::vec<2, u32>(i.glyph_location, i.band_info),
                           .color = i.color});
    return cmd.context().persistent.create_buffer_from_data(records, sg::buffer_usage::readonly_buffer);
}

cc::array<tg::pos3f> slug_quad_vertices(cc::span<slug_instance const> run)
{
    auto out = cc::array<tg::pos3f>::create_defaulted(run.size() * 6);
    for (auto i = isize(0); i < run.size(); ++i)
    {
        auto const& s = run[i];
        auto const place = [&](f32 x, f32 y)
        {
            auto const p = s.origin + tg::vec2f(s.em_to_object[0], s.em_to_object[1]) * x
                         + tg::vec2f(s.em_to_object[2], s.em_to_object[3]) * y;
            return tg::pos3f(p[0], p[1], 0.0f);
        };
        auto const lo = place(s.em_bounds[0], s.em_bounds[1]);
        auto const right = place(s.em_bounds[2], s.em_bounds[1]);
        auto const hi = place(s.em_bounds[2], s.em_bounds[3]);
        auto const up = place(s.em_bounds[0], s.em_bounds[3]);
        tg::pos3f const corners[] = {lo, right, hi, lo, hi, up};
        for (auto c = 0; c < 6; ++c)
            out[i * 6 + c] = corners[c];
    }
    return out;
}

sg::blas_handle build_slug_blas(sg::command_list& cmd, cc::span<slug_instance const> run, int hit_record_stride)
{
    CC_ASSERT(!run.empty(), "a BLAS needs at least one quad");
    // Transient, so the vertices live as long as the list that builds from them: a persistent buffer dropped here would
    // be released before the list is even submitted.
    auto const corners = slug_quad_vertices(run);
    auto const vertices = cmd.context().transient.create_buffer<tg::pos3f>(
        corners.size(), sg::buffer_usage::accel_structure_build_input | sg::buffer_usage::copy_dst);
    cmd.upload.data_to_buffer(vertices, cc::span<tg::pos3f const>(corners));
    auto const quads = sg::blas_triangles{.vertices = vertices.raw(), .vertex_count = run.size() * 6, .is_opaque = false};
    return cmd.raytracing.build_blas(cc::span<sg::blas_triangles const>(&quads, 1), sg::accel_build_flag::fast_trace,
                                     hit_record_stride);
}
} // namespace sr
