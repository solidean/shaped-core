#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh> // named_sampler
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>
#include <shaped-graphics/binding/pipeline_layout.hh> // pipeline_layout_description::groups
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>

namespace sg
{
void pipeline_cache::add_binding_group_layout_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, binding_group_layout_handle>> provider)
{
    _binding_group_layout_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_pipeline_layout_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, pipeline_layout_handle>> provider)
{
    _pipeline_layout_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_compute_pipeline_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, async_compute_pipeline>> provider)
{
    _compute_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_raytracing_pipeline_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, async_raytracing_pipeline>> provider)
{
    _raytracing_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_default_in_memory_providers(isize max_entries)
{
    _binding_group_layout_cache.add_default_in_memory_provider(max_entries);
    _pipeline_layout_cache.add_default_in_memory_provider(max_entries);
    _compute_cache.add_default_in_memory_provider(max_entries);
    _raytracing_cache.add_default_in_memory_provider(max_entries);
}

void pipeline_cache::apply_bookkeeping()
{
    _binding_group_layout_cache.apply_bookkeeping();
    _pipeline_layout_cache.apply_bookkeeping();
    _compute_cache.apply_bookkeeping();
    _raytracing_cache.apply_bookkeeping();
}

// The two layout keys ARE the layouts' structural identity — the same functions a backend stamps into the layout it
// creates, so this tier and any persistent one can never disagree about which layouts are the same.
cc::hash128 pipeline_cache::compute_binding_group_layout_key(cc::span<binding const> bindings,
                                                             cc::span<named_sampler const> static_samplers) const
{
    return impl::binding_group_layout_hash(bindings, static_samplers);
}

cc::hash128 pipeline_cache::compute_pipeline_layout_key(pipeline_layout_description const& desc) const
{
    return impl::pipeline_layout_hash(desc);
}

cc::hash128 pipeline_cache::compute_compute_pipeline_key(compute_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // shader content identity
    b.add(desc.shader.bytecode.span());
    b.add_string(desc.shader.entry_point);
    b.add_string(desc.shader.compiler.signature);
    // Pipeline-layout identity, which transitively covers its group layouts.
    b.add_pod(desc.layout != nullptr ? desc.layout->structural_hash() : cc::hash128{});
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_raytracing_pipeline_key(raytracing_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // Pipeline-layout identity, which transitively covers its group layouts.
    b.add_pod(desc.layout != nullptr ? desc.layout->structural_hash() : cc::hash128{});

    auto add_shader = [&b](compiled_shader const& s)
    {
        b.add(s.bytecode.span());
        b.add_string(s.entry_point);
        b.add_string(s.compiler.signature);
    };
    auto add_optional_shader = [&](cc::optional<compiled_shader> const& s)
    {
        b.add_pod(s.has_value());
        if (s.has_value())
            add_shader(s.value());
    };

    b.add_pod(u64(desc.raygen_shaders.size()));
    for (auto const& s : desc.raygen_shaders)
        add_shader(s);
    b.add_pod(u64(desc.miss_shaders.size()));
    for (auto const& s : desc.miss_shaders)
        add_shader(s);
    b.add_pod(u64(desc.callable_shaders.size()));
    for (auto const& s : desc.callable_shaders)
        add_shader(s);
    b.add_pod(u64(desc.hit_shaders.size()));
    for (auto const& h : desc.hit_shaders)
    {
        add_optional_shader(h.closest_hit);
        add_optional_shader(h.any_hit);
        add_optional_shader(h.intersection);
    }

    b.add_pod(desc.max_recursion_depth);
    b.add_pod(desc.max_payload_size);
    b.add_pod(desc.max_attribute_size);
    return cc::hash128::create(b.written_bytes(), 0);
}

binding_group_layout_handle pipeline_cache::acquire_binding_group_layout(context& ctx,
                                                                         cc::span<binding const> bindings,
                                                                         cc::span<named_sampler const> static_samplers)
{
    auto const key = this->compute_binding_group_layout_key(bindings, static_samplers);
    return _binding_group_layout_cache.acquire(
        key, [&] { return ctx.uncached.create_binding_group_layout(bindings, static_samplers); });
}

pipeline_layout_handle pipeline_cache::acquire_pipeline_layout(context& ctx, pipeline_layout_description const& desc)
{
    auto const key = this->compute_pipeline_layout_key(desc);
    return _pipeline_layout_cache.acquire(key, [&] { return ctx.uncached.create_pipeline_layout(desc); });
}

async_compute_pipeline pipeline_cache::acquire_compute_pipeline(context& ctx, compute_pipeline_description const& desc)
{
    auto const key = this->compute_compute_pipeline_key(desc);
    return _compute_cache.acquire(key,
                                  [&]() -> async_compute_pipeline
                                  {
                                      // The build frame runs later, possibly on a worker.
                                      // So own the shader copy + layout handle rather than the description's reference.
                                      return cc::make_async_scheduled<compute_pipeline_handle>(
                                          [ctx_ptr = &ctx, shader = compiled_shader(desc.shader), layout = desc.layout](
                                              cc::async_context<compute_pipeline_handle>& actx) -> cc::async_step_status
                                          {
                                              compute_pipeline_description const d = {.shader = shader, .layout = layout};
                                              auto res = ctx_ptr->uncached.try_create_compute_pipeline(d);
                                              if (res.has_error())
                                                  return actx.error(cc::move(res.error()));
                                              return actx.success(cc::move(res.value()));
                                          });
                                  });
}

async_raytracing_pipeline pipeline_cache::acquire_raytracing_pipeline(context& ctx,
                                                                      raytracing_pipeline_description const& desc)
{
    auto const key = this->compute_raytracing_pipeline_key(desc);
    return _raytracing_cache.acquire(
        key,
        [&]() -> async_raytracing_pipeline
        {
            // The build frame runs later, possibly on a worker.
            // So deep-copy the whole description, which owns its shader vectors + layout handle, rather than referencing the caller's.
            return cc::make_async_scheduled<raytracing_pipeline_handle>(
                [ctx_ptr = &ctx, d = raytracing_pipeline_description(desc)](
                    cc::async_context<raytracing_pipeline_handle>& actx) -> cc::async_step_status
                {
                    auto res = ctx_ptr->uncached.try_create_raytracing_pipeline(d);
                    if (res.has_error())
                        return actx.error(cc::move(res.error()));
                    return actx.success(cc::move(res.value()));
                });
        });
}
} // namespace sg
