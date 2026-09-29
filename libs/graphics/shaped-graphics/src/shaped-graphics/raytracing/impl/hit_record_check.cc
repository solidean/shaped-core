#include <clean-core/string/format.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/impl/hit_record_check.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

namespace sg::impl
{
cc::vector<cc::string> find_hit_record_mismatches(raytracing_shader_table const& table, tlas const& tlas, isize max_messages)
{
    auto messages = cc::vector<cc::string>();
    auto const records = table.hit_records();
    auto const ray_count = isize(table.ray_count());
    auto const* const pipeline = table.pipeline().get();

    auto const instances = instance_records_of(tlas);
    for (auto i = isize(0); i < instances.size() && messages.size() < max_messages; ++i)
    {
        auto const& instance = instances[i];
        if (instance.blas == nullptr || instance.mask == 0)
            continue;

        auto const& blas = *instance.blas;
        auto const aabbs = blas.geometry() == blas_geometry::aabbs;
        auto const stride = isize(blas.hit_record_stride());

        if (ray_count > 1 && stride != ray_count)
        {
            messages.push_back(cc::format("instance {} places a BLAS built with hit_record_stride {}, and the shader "
                                          "table "
                                          "traces {} ray types; metal bakes the stride, so build the BLAS with {}",
                                          i, stride, ray_count, ray_count));
            continue;
        }

        for (auto g = isize(0); g < isize(blas.geometry_count()) && messages.size() < max_messages; ++g)
            for (auto r = isize(0); r < ray_count && messages.size() < max_messages; ++r)
            {
                auto const record = isize(instance.hit_group_offset) + g * stride + r;
                if (record >= records.size())
                {
                    messages.push_back(cc::format("instance {} (hit_group_offset {}) reaches hit record {} for "
                                                  "geometry "
                                                  "{} and ray type {}, and the shader table holds {}",
                                                  i, instance.hit_group_offset, record, g, r, records.size()));
                    continue;
                }

                auto const procedural
                    = pipeline == nullptr ? cc::optional<bool>() : pipeline->is_procedural(records[record]);
                if (!procedural.has_value() || procedural.value() == aabbs)
                    continue;

                messages.push_back(
                    cc::format("instance {} (hit_group_offset {}) places a {} BLAS, and geometry {} with "
                               "ray type {} reaches hit record {}, whose hit group is {}",
                               i, instance.hit_group_offset, aabbs ? "procedural (AABB)" : "triangle", g, r, record,
                               procedural.value() ? "procedural (it has an intersection shader)"
                                                  : "a triangle group (it has no intersection shader)"));
            }
    }
    return messages;
}
} // namespace sg::impl
