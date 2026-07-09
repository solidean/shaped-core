#include "pipeline_cache.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/binding_group.hh> // named_sampler
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/compute_pipeline.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pipeline_layout.hh> // pipeline_layout_description::groups
#include <shaped-graphics/sampler.hh>

namespace sg
{
namespace
{
// Hash a sampler field by field (not add_pod on the whole struct — padding bytes would make the hash
// nondeterministic for logically-equal samplers).
void add_sampler(cc::byte_stream_builder& b, sampler const& s)
{
    b.add_pod(s.min_filter);
    b.add_pod(s.mag_filter);
    b.add_pod(s.mip_filter);
    b.add_pod(s.address_u);
    b.add_pod(s.address_v);
    b.add_pod(s.address_w);
    b.add_pod(s.mip_lod_bias);
    b.add_pod(s.max_anisotropy);
    b.add_pod(s.min_lod);
    b.add_pod(s.max_lod);
    b.add_optional(s.compare);
    b.add_pod(s.border_color);
}
} // namespace

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

void pipeline_cache::add_default_in_memory_providers(cc::isize max_entries)
{
    _binding_group_layout_cache.add_default_in_memory_provider(max_entries);
    _pipeline_layout_cache.add_default_in_memory_provider(max_entries);
    _compute_cache.add_default_in_memory_provider(max_entries);
}

void pipeline_cache::apply_bookkeeping()
{
    _binding_group_layout_cache.apply_bookkeeping();
    _pipeline_layout_cache.apply_bookkeeping();
    _compute_cache.apply_bookkeeping();
}

cc::hash128 pipeline_cache::compute_binding_group_layout_key(cc::span<binding const> bindings,
                                                             cc::span<named_sampler const> static_samplers) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(cc::u64(bindings.size()));
    for (auto const& bnd : bindings)
    {
        b.add_string(bnd.name);
        b.add_pod(bnd.set);
        b.add_pod(bnd.index);
        b.add_pod(bnd.count);
        b.add_pod(bnd.type);
        b.add_optional(bnd.block_size);
    }
    b.add_pod(cc::u64(static_samplers.size()));
    for (auto const& ns : static_samplers)
    {
        b.add_string(ns.name);
        add_sampler(b, ns.sampler);
    }
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_pipeline_layout_key(pipeline_layout_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(cc::u64(desc.groups.size()));
    for (auto const& g : desc.groups)
        // group-layout identity — pointer is stable because cached group layouts are shared/persistent
        b.add_pod(reinterpret_cast<cc::u64>(g.get()));
    // pipeline-level static samplers change the root signature, so they are part of the identity
    b.add_pod(cc::u64(desc.static_samplers.size()));
    for (auto const& bs : desc.static_samplers)
    {
        b.add_pod(bs.binding.set);
        b.add_pod(bs.binding.index);
        b.add_pod(bs.binding.count);
        b.add_pod(bs.binding.type);
        add_sampler(b, bs.sampler);
    }
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_compute_pipeline_key(compute_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // shader content identity
    b.add(desc.shader.bytecode.span());
    b.add_string(desc.shader.entry_point);
    b.add_string(desc.shader.compiler.signature);
    // pipeline-layout identity — pointer is stable because cached layouts are shared/persistent, and it
    // transitively covers its group layouts
    b.add_pod(reinterpret_cast<cc::u64>(desc.layout.get()));
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
                                      // The build frame runs later (possibly on a worker), so own the shader copy + layout handle
                                      // rather than the description's reference.
                                      return cc::make_async_scheduled<compute_pipeline_handle>(
                                          [ctx_ptr = &ctx, shader = compiled_shader(desc.shader), layout = desc.layout](
                                              cc::async_context& actx) -> cc::async_result<compute_pipeline_handle>
                                          {
                                              compute_pipeline_description const d{.shader = shader, .layout = layout};
                                              auto res = ctx_ptr->uncached.try_create_compute_pipeline(d);
                                              if (res.has_error())
                                                  return actx.error(cc::move(res.error()));
                                              return actx.success(cc::move(res.value()));
                                          });
                                  });
}
} // namespace sg
