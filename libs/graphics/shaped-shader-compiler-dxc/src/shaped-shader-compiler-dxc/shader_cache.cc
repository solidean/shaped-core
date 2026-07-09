#include "shader_cache.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <memory>

namespace ssc::dxc
{
namespace
{
// Per-thread DXC compiler: the compiler is one-per-thread / not thread-safe, so each worker that runs
// a compile frame lazily builds its own. A broken DXC install yields nullptr and the compile fails.
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
} // namespace

void shader_cache::add_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, sg::async_compiled_shader>> provider)
{
    _cache.add_provider(cc::move(provider));
}

void shader_cache::add_default_in_memory_provider(cc::isize max_entries)
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
    b.add_pod(cc::u64(options.defines.size()));
    for (auto const& d : options.defines)
        b.add_string(d);
    b.add_pod(cc::u64(options.extra_args.size()));
    for (auto const& a : options.extra_args)
        b.add_string(a);
    return cc::hash128::create(b.written_bytes(), 0);
}

sg::async_compiled_shader shader_cache::compile(shader_description const& desc, compile_options const& options)
{
    auto const key = this->compute_key(desc, options);

    return _cache.acquire(key,
                          [&]() -> sg::async_compiled_shader
                          {
                              // Copy desc + options into the deferred frame — the frame outlives this call.
                              return cc::make_async_scheduled<sg::compiled_shader>(
                                  [desc, options](cc::async_context& actx) -> cc::async_result<sg::compiled_shader>
                                  {
                                      compiler* comp = thread_local_compiler();
                                      if (comp == nullptr)
                                          return actx.error(cc::any_error(cc::string("failed to create DXC compiler")));

                                      auto res = comp->compile(desc, options);
                                      if (res.has_error())
                                          return actx.error(cc::move(res.error()));
                                      return actx.success(cc::move(res.value()));
                                  });
                          });
}
} // namespace ssc::dxc
