#include "pipeline_cache.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/compute_pipeline.hh>
#include <shaped-graphics/context.hh>

namespace sg
{
void pipeline_cache::add_binding_layout_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, binding_layout_handle>> provider)
{
    _layout_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_compute_pipeline_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, async_compute_pipeline>> provider)
{
    _compute_cache.add_provider(cc::move(provider));
}

void pipeline_cache::add_default_in_memory_providers(cc::isize max_entries)
{
    _layout_cache.add_default_in_memory_provider(max_entries);
    _compute_cache.add_default_in_memory_provider(max_entries);
}

void pipeline_cache::apply_bookkeeping()
{
    _layout_cache.apply_bookkeeping();
    _compute_cache.apply_bookkeeping();
}

cc::hash128 pipeline_cache::compute_binding_layout_key(cc::span<binding const> bindings) const
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
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_compute_pipeline_key(compute_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // shader content identity
    b.add(desc.shader.bytecode.span());
    b.add_string(desc.shader.entry_point);
    b.add_string(desc.shader.compiler.signature);
    // layout identity — pointer is stable because cached layouts are shared/persistent
    b.add_pod(reinterpret_cast<cc::u64>(desc.layout.get()));
    return cc::hash128::create(b.written_bytes(), 0);
}

binding_layout_handle pipeline_cache::acquire_binding_layout(context& ctx, cc::span<binding const> bindings)
{
    auto const key = this->compute_binding_layout_key(bindings);
    return _layout_cache.acquire(key, [&] { return ctx.persistent.create_binding_layout(bindings); });
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
                                              compute_pipeline_description d{.shader = shader, .layout = layout};
                                              auto res = ctx_ptr->persistent.try_create_compute_pipeline(d);
                                              if (res.has_error())
                                                  return actx.error(cc::move(res.error()));
                                              return actx.success(cc::move(res.value()));
                                          });
                                  });
}
} // namespace sg
