#include "shader_cache.hh"

#include <blob-cache/blob_cache.hh>
#include <blob-cache/default_cache.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // including it is what makes compile_shader a coroutine
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/binding/impl/shader_codec.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <memory>

namespace ssc::dxc
{
namespace
{
// Per-thread DXC compiler: the compiler is one-per-thread / not thread-safe, so each worker that runs a compile frame lazily builds its own.
// A broken DXC install yields nullptr and the compile fails.
compiler* thread_local_compiler()
{
    static thread_local std::unique_ptr<compiler> const instance = []() -> std::unique_ptr<compiler>
    {
        auto r = compiler::create();
        if (r.has_error())
            return nullptr;
        return std::make_unique<compiler>(cc::move(r.value()));
    }();
    return instance.get();
}

/// Bumped when what goes into a persistent shader entry changes shape.
/// The codec carries its own version for the bytes; this is about the entry.
constexpr auto k_shader_blob_version = bcache::version(1);

/// The DXC version this process compiles with, or empty where DXC is unusable.
/// Reading it builds this thread's compiler if it has none, which the compile was about to do anyway.
cc::string_view compiler_version()
{
    auto const* const comp = thread_local_compiler();
    return comp != nullptr ? comp->version() : cc::string_view();
}

cc::result<sg::compiled_shader> compile_now(shader_description const& desc, compile_options const& options)
{
    compiler* const comp = thread_local_compiler();
    if (comp == nullptr)
        return cc::error("failed to create DXC compiler");
    return comp->compile(desc, options);
}

/// One compile, with the persistent tier in front of it.
///
/// Unlike a pipeline, the cacheable product and the expensive product are the same bytes here, so this is a plain
/// acquire with no slot and no refresh: whatever comes back is decoded, and a miss compiles and encodes.
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
        // The codec refuses anything doubtful, which is what makes this the only outcome corruption can have.
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
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(desc.source);
    b.add_string(desc.entry_point);
    b.add_pod(desc.stage);
    b.add_pod(desc.model);
    b.add_pod(options.target);
    b.add_pod(options.optimization);
    b.add_bool(options.debug_info);
    b.add_bool(options.warnings_as_errors);
    b.add_pod(u64(options.defines.size()));
    for (auto const& d : options.defines)
        b.add_string(d);
    b.add_pod(u64(options.extra_args.size()));
    for (auto const& a : options.extra_args)
        b.add_string(a);

    // The DXC version, so an upgrade does not keep serving the previous compiler's DXIL.
    // Irrelevant to an in-memory tier that dies with the process, and load-bearing for one that does not.
    b.add_string(compiler_version());

    return cc::hash128::create(b.written_bytes(), 0);
}

bcache::cache_key shader_cache::persistent_key(cc::hash128 compile_key) const
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(compile_key);
    return {.space = bcache::cache_namespace("ssc.dxc.shader"),
            .key = bcache::logical_key::create_from_hash(cc::hash256::create(b.written_bytes())),
            .version = k_shader_blob_version};
}

void shader_cache::set_blob_cache(bcache::blob_cache* cache)
{
    _blob_cache = cache;
}

bcache::blob_cache* shader_cache::resolve_blob_cache()
{
    // A compile parks on the store, so it needs somewhere to resume.
    // With nowhere to route, the tier is skipped rather than parking on a node whose completion could not wake it.
    if (cc::async_scheduler::current_or_null() == nullptr && cc::async_scheduler::default_or_null() == nullptr)
        return nullptr;

    if (!_blob_cache.has_value())
        _blob_cache = &bcache::default_cache();
    return _blob_cache.value();
}

sg::async_compiled_shader shader_cache::compile(shader_description const& desc, compile_options const& options)
{
    auto const key = this->compute_key(desc, options);

    return _cache.acquire(key,
                          [&]() -> sg::async_compiled_shader
                          {
                              // Copy desc + options into the deferred coroutine — it outlives this call.
                              auto node
                                  = compile_shader(desc, options, this->resolve_blob_cache(), this->persistent_key(key));

                              // A coroutine is cold; this tier has always handed back a scheduled node.
                              return cc::async_start(cc::move(node));
                          });
}
} // namespace ssc::dxc
