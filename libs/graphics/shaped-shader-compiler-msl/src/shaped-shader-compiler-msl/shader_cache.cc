#include "shader_cache.hh"

#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes compile_shader a coroutine
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/impl/shader_codec.hh>
#include <shaped-graphics/context/cold_caches.hh>
#include <shaped-shader-compiler-msl/compiler.hh>

#include <memory>

namespace ssc::msl
{
namespace
{
/// The process's one compiler, created on first use.
/// `compiler::compile` only reads what `create` resolved, so every thread shares it, and `xcrun` runs once.
compiler const* shared_compiler()
{
    static auto const instance = compiler::create();
    return instance.has_value() ? &instance.value() : nullptr;
}

/// Moves with the codec, so a new codec never reads an old entry at all.
constexpr auto k_shader_blob_version = bcache::version(sg::impl::k_shader_codec_version);

cc::result<sg::compiled_shader> compile_now(shader_description const& desc, compile_options const& options)
{
    auto const* const comp = shared_compiler();
    if (comp == nullptr)
        return cc::error("failed to create the metal shader compiler");
    return comp->compile(desc, options);
}

/// One compile, with the persistent tier in front of it.
///
/// Parameters are by value: a coroutine captures them by declared type, so a reference would dangle across the first suspend.
sg::async_compiled_shader compile_shader(shader_description desc,
                                         compile_options options,
                                         bcache::blob_cache* store,
                                         bcache::cache_key key)
{
    if (store != nullptr)
    {
        auto compute = [desc, options]() -> cc::shared_async<bcache::blob>
        {
            return cc::make_async_lazy<bcache::blob>(
                [desc, options](cc::async_context<bcache::blob>& actx) -> cc::async_step_status
                {
                    auto built = compile_now(desc, options);
                    if (built.has_error())
                        return actx.error(cc::move(built.error()));
                    return actx.success(cc::make_pinned_data(sg::impl::encode_compiled_shader(built.value())));
                });
        };

        // A plain await: the only failure acquire surfaces is the compile's own, and that one must reach the caller.
        auto const bytes = co_await store->acquire(key, cc::move(compute));

        if (auto decoded = sg::impl::decode_compiled_shader(bytes); decoded.has_value())
            co_return cc::move(decoded.value());

        // Bytes we cannot read are a miss like any other, so fall through and compile.
    }

    auto built = compile_now(desc, options);
    if (built.has_error())
        co_await cc::async_fail(cc::move(built.error()));
    co_return cc::move(built.value());
}
} // namespace

void shader_cache::add_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, sg::async_compiled_shader>> provider)
{
    _cache.add_provider(cc::move(provider));
}

void shader_cache::add_default_in_memory_provider(isize max_entries)
{
    _cache.add_default_in_memory_provider(max_entries);
}

void shader_cache::apply_bookkeeping()
{
    _cache.apply_bookkeeping();
}

cc::hash128 shader_cache::compute_key(shader_description const& desc, compile_options const& options) const
{
    auto const* const comp = shared_compiler();
    auto const has_toolchain = comp != nullptr && comp->toolchain().is_available;

    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(desc.source);
    b.add_string(desc.entry_point);
    b.add_pod(desc.stage);
    b.add_bool(desc.workgroup_size.has_value());
    if (desc.workgroup_size.has_value())
    {
        b.add_pod(desc.workgroup_size.value().x);
        b.add_pod(desc.workgroup_size.value().y);
        b.add_pod(desc.workgroup_size.value().z);
    }
    b.add_pod(options.artifact);
    b.add_pod(options.optimization);
    b.add_bool(options.debug_info);
    b.add_bool(options.warnings_as_errors);
    b.add_string(options.language_version);
    b.add_pod(u64(options.defines.size()));
    for (auto const& d : options.defines)
        b.add_string(d);
    b.add_pod(u64(options.extra_args.size()));
    for (auto const& a : options.extra_args)
        b.add_string(a);

    // Which arm runs is known before the compile, and a metallib and a source blob of one shader are different entries.
    b.add_bool(options.artifact == artifact_kind::metallib
               || (options.artifact == artifact_kind::automatic && has_toolchain));

    // Apple ships the toolchain as an updatable component, so an upgrade must not keep serving the old one's metallib.
    b.add_string(comp != nullptr ? cc::string_view(comp->toolchain().version) : cc::string_view());

    return cc::hash128::create(b.written_bytes(), 0);
}

bcache::cache_key shader_cache::persistent_key(cc::hash128 compile_key) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(compile_key);
    return {.space = bcache::cache_namespace("ssc.msl.shader"),
            .key = bcache::logical_key::create_from_hash(cc::hash256::create(b.written_bytes())),
            .version = k_shader_blob_version};
}

void shader_cache::set_blob_cache(bcache::blob_cache* cache)
{
    _blob_cache = cache;
}

bcache::blob_cache* shader_cache::resolve_blob_cache()
{
    // With nowhere to route, the tier is skipped rather than parking on a node whose completion could not wake it.
    if (!cc::impl::async_can_schedule_here())
        return nullptr;
    // Cold only instead of the default store: one set explicitly is a choice, and a test of this tier depends on it.
    if (!_blob_cache.has_value() && sg::cold_caches_from_environment().shaders)
        return nullptr;

    if (!_blob_cache.has_value())
        _blob_cache = &bcache::default_cache();
    return _blob_cache.value();
}

sg::async_compiled_shader shader_cache::compile(shader_description const& desc, compile_options const& options)
{
    auto const key = this->compute_key(desc, options);

    // A miss is the lambda RUNNING: the in-memory tier hands back an existing node on a hit and never calls it.
    CC_RECORD_ACCUM("ssc.cache.requests", cc::rec::unit_count, 1);

    return _cache.acquire(key,
                          [&]() -> sg::async_compiled_shader
                          {
                              CC_RECORD_ACCUM("ssc.cache.misses", cc::rec::unit_count, 1);

                              // Copies desc and options into the cold coroutine, which outlives this call.
                              auto node
                                  = compile_shader(desc, options, this->resolve_blob_cache(), this->persistent_key(key));
                              return _backlog.start(cc::move(node));
                          });
}
} // namespace ssc::msl
