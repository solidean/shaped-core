#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes build_compute_pipeline a coroutine
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
namespace
{
/// Bumped when what goes into a persistent pipeline entry changes shape.
/// The adapter and driver live in the key itself; this is only ever about OUR encoding.
constexpr auto k_pipeline_blob_version = bcache::version(1);

/// The namespace fragment naming a backend, so two backends' blobs can never answer each other's lookups.
/// backend_kind is explicitly not a closed set, so an unnamed one shares the "unknown" partition rather than silently
/// colliding with dx12's.
cc::string_view name_of(backend_kind backend)
{
    switch (backend)
    {
    case backend_kind::dx12:
        return "dx12";
    case backend_kind::vulkan:
        return "vulkan";
    default:
        return "unknown";
    }
}

/// Builds a compute pipeline, letting a persistent PSO blob accelerate it.
///
/// `acquire` rather than get-then-put, so the whole lookup-build-store pipeline singleflights and identical blobs
/// deduplicate by content — without loading anything at startup.
///
/// The wrinkle it has to solve: the singleflight's `compute` produces the BLOB, while the caller wants the PIPELINE.
/// A slot only the winner fills resolves that, and doubles as the hit/miss signal acquire does not otherwise expose.
///
/// Parameters are by value: a coroutine captures them by declared type, so a reference would dangle across the first suspend.
cc::shared_async<compute_pipeline_handle> build_compute_pipeline(context* ctx,
                                                                 compiled_shader shader,
                                                                 pipeline_layout_handle layout,
                                                                 bcache::blob_cache* store,
                                                                 bcache::cache_key key)
{
    auto const create = [&](bcache::blob blob)
    {
        return ctx->uncached.try_create_compute_pipeline(
            {.shader = shader, .layout = layout, .cached_pipeline = cc::move(blob)});
    };

    if (store == nullptr)
    {
        auto plain = create({});
        if (plain.has_error())
            co_await cc::async_fail(cc::move(plain.error()));
        co_return cc::move(plain.value());
    }

    // Filled only by the singleflight winner, so "is it filled" IS "this was a miss".
    auto const produced = std::make_shared<cc::optional<compute_pipeline_handle>>();

    auto compute = [ctx, shader, layout, produced]() -> cc::shared_async<bcache::blob>
    {
        return cc::make_async_lazy<bcache::blob>(
            [ctx, shader, layout, produced](cc::async_context<bcache::blob>& actx) -> cc::async_step_status
            {
                auto built = ctx->uncached.try_create_compute_pipeline({.shader = shader, .layout = layout});
                if (built.has_error())
                    return actx.error(cc::move(built.error()));

                *produced = built.value();
                // Empty where the backend serializes nothing (dx12 state objects); the entry is then a tiny placeholder
                // and every later run takes the plain build path, which is the same work it would have done anyway.
                return actx.success(built.value()->cached_pipeline_data());
            });
    };

    // A plain await: the only failure acquire surfaces is the compute's own, and that one must reach the caller.
    // Every cache failure is already a miss by then.
    auto const cached = co_await store->acquire(key, cc::move(compute));

    if (produced->has_value())
        co_return cc::move(produced->value()); // we built it, so there is nothing to rebuild from the blob

    auto seeded = create(cached);
    if (seeded.has_error())
        co_await cc::async_fail(cc::move(seeded.error()));

    auto pipeline = cc::move(seeded.value());

    // The driver refused the blob, so the entry is stale for this adapter and driver — replace it rather than pay the
    // rejection on every future run.
    // Entries are immutable, so a replacement is invalidate-then-put; both are enqueued on one actor, so they stay ordered.
    //
    // Never decided by comparing bytes: a real driver re-serializes an accepted blob to different bytes, so a byte test
    // would rewrite every entry on every run.
    if (!cached.empty() && !pipeline->used_cached_pipeline())
    {
        auto fresh = pipeline->cached_pipeline_data();
        if (!fresh.empty())
        {
            (void)store->invalidate(key);
            (void)store->put(key, cc::move(fresh));
        }
    }

    co_return pipeline;
}
} // namespace

void pipeline_cache::set_blob_cache(bcache::blob_cache* cache)
{
    _blob_cache = cache;
}

bcache::blob_cache* pipeline_cache::resolve_blob_cache()
{
    // A build parks on the store, so it needs somewhere to resume.
    // With nowhere to route, the tier is skipped rather than parking on a node whose completion could not wake it —
    // a cache may never change what a caller gets, only how fast.
    if (cc::async_scheduler::current_or_null() == nullptr && cc::async_scheduler::default_or_null() == nullptr)
        return nullptr;

    // Resolved lazily so that merely creating a context never opens a cache file.
    if (!_blob_cache.has_value())
        _blob_cache = &bcache::default_cache();
    return _blob_cache.value();
}

bool pipeline_cache::pump()
{
    // Only what is already in use: resolving here would open the default store just because somebody pumped.
    if (!_blob_cache.has_value() || _blob_cache.value() == nullptr)
        return false;
    return _blob_cache.value()->pump();
}

bcache::cache_key pipeline_cache::persistent_key(context& ctx, cc::hash128 pipeline_key, cc::string_view kind) const
{
    auto const& adapter = ctx.adapter();

    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(pipeline_key);
    b.add_pod(adapter.vendor_id);
    b.add_pod(adapter.device_id);
    b.add_string(adapter.driver_version);
    b.add_bool(adapter.is_software);

    return {.space = bcache::cache_namespace(cc::format("sg.{}.{}", kind, name_of(ctx.backend()))),
            .key = bcache::logical_key::create_from_hash(cc::hash256::create(b.written_bytes())),
            .version = k_pipeline_blob_version};
}

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
                                      // The build runs later, possibly on a worker, so it owns the shader copy and layout handle rather than
                                      // the description's reference.
                                      auto node = build_compute_pipeline(&ctx, compiled_shader(desc.shader),
                                                                         desc.layout, this->resolve_blob_cache(),
                                                                         this->persistent_key(ctx, key, "pso"));

                                      // A coroutine is cold; this tier has always handed back a scheduled node.
                                      return cc::async_start(cc::move(node));
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
