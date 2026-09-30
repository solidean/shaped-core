#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>

/// Metal implementation of sg::raytracing_shader_table.
///
/// **Four tables, not one**, and that is a language constraint rather than a preference.
/// MSL's `visible_function_table<T>` is typed by the function signature, so one table cannot hold miss, closest-hit
/// and callable functions — their signatures differ, and the compiler rejects calling one table two ways.
/// So each of sg's index spaces gets a table of its own and `miss_index`, `hit_index` and `callable_index` are used
/// verbatim, with no base to add and nothing for a kernel to compute wrongly.
///
/// The intersection table is the other kind: it holds what runs *during* traversal, and Metal indexes it by the
/// instance's `intersectionFunctionTableOffset` plus the geometry descriptor's own — sg's `hit_group_offset` plus the
/// geometry index times the BLAS's `hit_record_stride`, which is where DXR's first two hit-index contributions land.
/// **DXR's third, the per-`TraceRay` ray contribution, is a choice of table**: there is one intersection table per ray
/// type, and table r's slot s holds the traversal function of hit record s + r.
/// A kernel tracing ray type r with table r therefore reaches record `hit_group_offset + g * stride + r`, as DXR would.
/// That covers a ray type fixed at each call site, which is what SGL generates; a ray contribution computed at run time
/// has no counterpart — see libs/graphics/shaped-graphics/docs/concepts/raytracing-pipeline.md.
///
/// **A kernel whose intersection table holds any triangle group must declare it
/// `intersection_function_table<instancing, triangle_data>`**, because the opaque triangle default is installed with
/// exactly that signature and Metal requires the two to agree.
///
/// **They reach a kernel through `sg::reserved_binding_group`**, as members of that group's argument buffer:
/// `[[id(0)]]` ray type 0's intersection table, `[[id(1)]]` miss, `[[id(2)]]` closest-hit, `[[id(3)]]` callable, and
/// ray type r >= 1's intersection table at `[[id(3 + r)]]`.
/// The closest-hit table stays indexed by hit record, which the kernel computes from the same three terms.
/// The reservation already existed for exactly this — see sg::reserved_binding_group — so nothing about what
/// `group_index` means changes, and a caller still gets groups 0 to 2 on every backend.
///
/// **One set per raygen**, because a function handle is minted from a specific pipeline state and this pipeline has
/// one state per raygen shader.
class sg::backend::metal::metal_raytracing_shader_table final : public sg::raytracing_shader_table
{
public:
    /// Everything one raygen entry of this table dispatches with.
    struct raygen_binding
    {
        MTL::ComputePipelineState* state = nullptr; ///< borrowed from the pipeline, which outlives this table
        cc::vector<MTL::IntersectionFunctionTable*> intersections; ///< one per ray type, by ray type
        MTL::VisibleFunctionTable* miss = nullptr;
        MTL::VisibleFunctionTable* closest_hit = nullptr;
        MTL::VisibleFunctionTable* callable = nullptr;
        MTL::Buffer* arguments = nullptr; ///< the reserved group's argument buffer, holding the tables' resource ids
    };

    metal_raytracing_shader_table(metal_context& ctx,
                                  sg::raytracing_shader_table_description const& desc,
                                  cc::vector<raygen_binding> raygens)
      : sg::raytracing_shader_table(desc), _ctx(ctx), _raygens(cc::move(raygens))
    {
    }

    ~metal_raytracing_shader_table() override;

    [[nodiscard]] raygen_binding const& binding_for(sg::raygen_index raygen) const
    {
        CC_ASSERT(u32(raygen) < u32(_raygens.size()), "raygen index is out of this shader table's range");
        return _raygens[isize(u32(raygen))];
    }

private:
    metal_context& _ctx;
    cc::vector<raygen_binding> _raygens;
};
