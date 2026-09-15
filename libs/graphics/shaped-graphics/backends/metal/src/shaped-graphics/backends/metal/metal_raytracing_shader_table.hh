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
/// instance's `intersectionFunctionTableOffset` — which is exactly sg's `hit_group_offset`.
///
/// **They reach a kernel through `sg::reserved_binding_group`**, as four members of that group's argument buffer:
/// `[[id(0)]]` intersection, `[[id(1)]]` miss, `[[id(2)]]` closest-hit, `[[id(3)]]` callable.
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
        MTL::IntersectionFunctionTable* intersection = nullptr;
        MTL::VisibleFunctionTable* miss = nullptr;
        MTL::VisibleFunctionTable* closest_hit = nullptr;
        MTL::VisibleFunctionTable* callable = nullptr;
        MTL::Buffer* arguments = nullptr; ///< the reserved group's argument buffer, holding the four resource ids
    };

    metal_raytracing_shader_table(metal_context& ctx,
                                  sg::raytracing_pipeline_handle pipeline,
                                  cc::vector<raygen_binding> raygens)
      : sg::raytracing_shader_table(cc::move(pipeline)), _ctx(ctx), _raygens(cc::move(raygens))
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
