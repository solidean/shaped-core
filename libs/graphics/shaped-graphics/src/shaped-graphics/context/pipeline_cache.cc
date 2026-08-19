#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh> // named_sampler
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/pipeline_layout.hh> // pipeline_layout_description::groups
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>

namespace sg
{
namespace
{
// Hash a sampler field by field.
// Not add_pod over the whole struct: padding bytes would make the hash nondeterministic for logically-equal samplers.
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

// A shader's content identity: the bytecode plus what the compiler folded into it.
// `stage` and `format` are implied by the slot the shader sits in and by the bytecode itself.
void add_shader(cc::byte_stream_builder& b, compiled_shader const& s)
{
    b.add(s.bytecode.span());
    b.add_string(s.entry_point);
    b.add_string(s.compiler.signature);
}

void add_optional_shader(cc::byte_stream_builder& b, cc::optional<compiled_shader> const& s)
{
    b.add_bool(s.has_value());
    if (s.has_value())
        add_shader(b, s.value());
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

void pipeline_cache::add_raster_pipeline_provider(
    std::shared_ptr<cc::key_value_provider<cc::hash128, async_raster_pipeline>> provider)
{
    _raster_cache.add_provider(cc::move(provider));
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
    _raster_cache.add_default_in_memory_provider(max_entries);
    _raytracing_cache.add_default_in_memory_provider(max_entries);
}

void pipeline_cache::apply_bookkeeping()
{
    _binding_group_layout_cache.apply_bookkeeping();
    _pipeline_layout_cache.apply_bookkeeping();
    _compute_cache.apply_bookkeeping();
    _raster_cache.apply_bookkeeping();
    _raytracing_cache.apply_bookkeeping();
}

cc::hash128 pipeline_cache::compute_binding_group_layout_key(cc::span<binding const> bindings,
                                                             cc::span<named_sampler const> static_samplers) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(u64(bindings.size()));
    for (auto const& bnd : bindings)
    {
        b.add_string(bnd.name);
        b.add_pod(bnd.set);
        b.add_pod(bnd.index);
        b.add_pod(bnd.count);
        b.add_pod(bnd.type);
        b.add_optional(bnd.block_size);
        b.add_optional(bnd.texture_dimension);
    }
    b.add_pod(u64(static_samplers.size()));
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
    b.add_pod(u64(desc.groups.size()));
    for (auto const& g : desc.groups)
        // group-layout identity — pointer is stable because cached group layouts are shared/persistent
        b.add_pod(reinterpret_cast<u64>(g.get()));
    // pipeline-level static samplers change the root signature, so they are part of the identity
    b.add_pod(u64(desc.static_samplers.size()));
    for (auto const& bs : desc.static_samplers)
    {
        b.add_pod(bs.binding.set);
        b.add_pod(bs.binding.index);
        b.add_pod(bs.binding.count);
        b.add_pod(bs.binding.type);
        add_sampler(b, bs.sampler);
    }
    // inline constants add a 32-bit-constants root parameter, so they are part of the identity too
    b.add_pod(desc.inline_constants.has_value());
    if (desc.inline_constants.has_value())
    {
        auto const& ic = desc.inline_constants.value();
        b.add_pod(ic.set);
        b.add_pod(ic.index);
        b.add_pod(ic.count);
        b.add_pod(ic.type);
        b.add_optional(ic.block_size);
    }
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_compute_pipeline_key(compute_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    add_shader(b, desc.shader);
    // pipeline-layout identity — the pointer is stable because cached layouts are shared and persistent.
    // It transitively covers its group layouts.
    b.add_pod(reinterpret_cast<u64>(desc.layout.get()));
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_raster_pipeline_key(raster_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // pipeline-layout identity — pointer is stable because cached layouts are shared/persistent
    b.add_pod(reinterpret_cast<u64>(desc.layout.get()));

    add_shader(b, desc.vertex_shader);
    add_optional_shader(b, desc.fragment_shader);
    add_optional_shader(b, desc.tessellation_control_shader);
    add_optional_shader(b, desc.tessellation_evaluation_shader);
    add_optional_shader(b, desc.geometry_shader);

    b.add_pod(u64(desc.vertex_input.slots.size()));
    for (auto const& s : desc.vertex_input.slots)
    {
        b.add_pod(s.stride);
        b.add_bool(s.per_instance);
    }
    b.add_pod(u64(desc.vertex_input.attributes.size()));
    for (auto const& a : desc.vertex_input.attributes)
    {
        b.add_string(a.semantic);
        b.add_pod(a.semantic_index);
        b.add_pod(a.format);
        b.add_pod(a.offset);
        b.add_pod(a.slot);
    }

    b.add_pod(desc.topology);
    b.add_pod(desc.patch_control_points);

    // Field by field, like add_sampler: padding bytes would make the hash nondeterministic for logically-equal states.
    b.add_pod(desc.rasterization.fill);
    b.add_pod(desc.rasterization.cull);
    b.add_pod(desc.rasterization.front);
    b.add_bool(desc.rasterization.depth_clip_enabled);
    b.add_pod(desc.rasterization.depth_bias);
    b.add_pod(desc.rasterization.depth_bias_slope);
    b.add_pod(desc.rasterization.depth_bias_clamp);

    auto const add_stencil_face = [&b](stencil_face const& f)
    {
        b.add_pod(f.fail);
        b.add_pod(f.depth_fail);
        b.add_pod(f.pass);
        b.add_pod(f.compare);
    };
    b.add_bool(desc.depth_stencil.depth_test);
    b.add_bool(desc.depth_stencil.depth_write);
    b.add_pod(desc.depth_stencil.depth_compare);
    b.add_bool(desc.depth_stencil.stencil_test);
    b.add_pod(desc.depth_stencil.stencil_read_mask);
    b.add_pod(desc.depth_stencil.stencil_write_mask);
    add_stencil_face(desc.depth_stencil.front);
    add_stencil_face(desc.depth_stencil.back);

    b.add_pod(u64(desc.color_targets.size()));
    for (auto const& t : desc.color_targets)
    {
        b.add_pod(t.format);
        b.add_bool(t.blend.has_value());
        if (t.blend.has_value())
        {
            auto const add_blend_component = [&b](blend_component const& c)
            {
                b.add_pod(c.source);
                b.add_pod(c.target);
                b.add_pod(c.op);
            };
            add_blend_component(t.blend.value().color);
            add_blend_component(t.blend.value().alpha);
        }
        b.add_pod(t.write_mask.bits);
    }

    b.add_pod(desc.depth_stencil_format);
    b.add_pod(desc.sample_count);
    // desc.cached_pipeline is deliberately NOT part of the key: it is a best-effort driver blob, so seeding a build with
    // one must still share the PSO of an otherwise-identical description.
    return cc::hash128::create(b.written_bytes(), 0);
}

cc::hash128 pipeline_cache::compute_raytracing_pipeline_key(raytracing_pipeline_description const& desc) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    // pipeline-layout identity — pointer is stable because cached layouts are shared/persistent
    b.add_pod(reinterpret_cast<u64>(desc.layout.get()));

    b.add_pod(u64(desc.raygen_shaders.size()));
    for (auto const& s : desc.raygen_shaders)
        add_shader(b, s);
    b.add_pod(u64(desc.miss_shaders.size()));
    for (auto const& s : desc.miss_shaders)
        add_shader(b, s);
    b.add_pod(u64(desc.callable_shaders.size()));
    for (auto const& s : desc.callable_shaders)
        add_shader(b, s);
    b.add_pod(u64(desc.hit_shaders.size()));
    for (auto const& h : desc.hit_shaders)
    {
        add_optional_shader(b, h.closest_hit);
        add_optional_shader(b, h.any_hit);
        add_optional_shader(b, h.intersection);
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

async_raster_pipeline pipeline_cache::acquire_raster_pipeline(context& ctx, raster_pipeline_description const& desc)
{
    auto const key = this->compute_raster_pipeline_key(desc);
    return _raster_cache.acquire(key,
                                 [&]() -> async_raster_pipeline
                                 {
                                     // The build frame runs later, possibly on a worker.
                                     // So deep-copy the whole description, which owns its shaders + layout handle, rather than referencing the caller's.
                                     return cc::make_async_scheduled<raster_pipeline_handle>(
                                         [ctx_ptr = &ctx, d = raster_pipeline_description(desc)](
                                             cc::async_context<raster_pipeline_handle>& actx) -> cc::async_step_status
                                         {
                                             auto res = ctx_ptr->uncached.try_create_raster_pipeline(d);
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
