#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>

/// Metal implementation of sg::raytracing_pipeline.
///
/// **There is no MTL4 ray-tracing pipeline**, so this is not a state object with a Metal name.
/// DXR gives the driver a set of shaders and lets it schedule them; Metal dispatches an ordinary compute kernel that
/// calls `intersector` itself, with function tables supplying what traversal and the kernel call back into.
/// So a raygen shader is not something a pipeline dispatches — it **is** the kernel.
///
/// **One MTL4 compute pipeline per registered raygen shader**, therefore, each dynamically linked with every hit, miss
/// and callable function the description registered.
/// Dynamic linking rather than static: `raytracing_pipeline_description` already owns every shader, so static would
/// fit — but it would drop the property the handle-to-index split exists for, one pipeline backing several tables with
/// different function sets.
/// It is also what `maxCallStackDepth` belongs to, which is where `max_recursion_depth` lands.
///
/// The linked functions are kept as `MTL4::BinaryFunction`s because that is what a function handle is minted from, and
/// a handle is per pipeline state — so a shader table built from this pipeline is built per raygen.
class sg::backend::metal::metal_raytracing_pipeline final : public sg::raytracing_pipeline
{
public:
    /// One registered hit group, split the way Metal splits it.
    ///
    /// `intersection` and `any_hit` run *during* traversal, so they belong in an intersection function table.
    /// `closest_hit` runs after it and is called by the kernel, so it is a visible function like a miss shader.
    /// One sg hit group therefore lands in both tables at the same index, which is what keeps `hit_index` meaning one
    /// thing.
    struct hit_group
    {
        MTL4::BinaryFunction* intersection = nullptr; ///< null for a triangle group, which Metal intersects itself
        MTL4::BinaryFunction* any_hit = nullptr;
        MTL4::BinaryFunction* closest_hit = nullptr;
        bool is_procedural = false; ///< an intersection shader was registered, so the group refines its own AABBs
    };

    metal_raytracing_pipeline(metal_context& ctx,
                              cc::vector<MTL::ComputePipelineState*> raygen_states,
                              cc::vector<MTL4::BinaryFunction*> miss_functions,
                              cc::vector<MTL4::BinaryFunction*> callable_functions,
                              cc::vector<hit_group> hit_groups,
                              cc::vector<MTL::Library*> libraries,
                              sg::pipeline_layout_handle layout)
      : _ctx(ctx),
        _raygen_states(cc::move(raygen_states)),
        _miss_functions(cc::move(miss_functions)),
        _callable_functions(cc::move(callable_functions)),
        _hit_groups(cc::move(hit_groups)),
        _libraries(cc::move(libraries)),
        _layout(cc::move(layout))
    {
    }

    ~metal_raytracing_pipeline() override { release_backend_objects(); }

    void release_backend_objects() override;

    /// The compute pipeline state a raygen shader registered at `handle` became.
    [[nodiscard]] MTL::ComputePipelineState* raygen_state(sg::raygen_shader_handle handle) const
    {
        CC_ASSERT(u32(handle) < u32(_raygen_states.size()), "raygen shader handle is out of this pipeline's range");
        return _raygen_states[isize(u32(handle))];
    }

    [[nodiscard]] cc::span<MTL4::BinaryFunction* const> miss_functions() const { return _miss_functions; }
    [[nodiscard]] cc::span<MTL4::BinaryFunction* const> callable_functions() const { return _callable_functions; }
    [[nodiscard]] cc::span<hit_group const> hit_groups() const { return _hit_groups; }
    [[nodiscard]] sg::pipeline_layout_handle const& layout() const { return _layout; }

    /// Metal has no serialized state-object blob, the way it has none for a compute pipeline.
    /// MTL4Archive is a per-compiler store where sg's surface is one blob per pipeline — see
    /// libs/graphics/shaped-graphics/docs/TODO.md.
    [[nodiscard]] cc::pinned_data<byte const> cached_pipeline_data() const override { return {}; }

private:
    metal_context& _ctx;
    cc::vector<MTL::ComputePipelineState*> _raygen_states;
    cc::vector<MTL4::BinaryFunction*> _miss_functions;
    cc::vector<MTL4::BinaryFunction*> _callable_functions;
    cc::vector<hit_group> _hit_groups;

    /// Held for the life of the pipeline: a binary function is minted from a library's function, and releasing the
    /// library out from under it is not something Metal promises to survive.
    cc::vector<MTL::Library*> _libraries;

    sg::pipeline_layout_handle _layout;
};
