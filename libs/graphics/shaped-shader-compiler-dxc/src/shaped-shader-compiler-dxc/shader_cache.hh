#pragma once

#include <clean-core/common/hash128.hh>
#include <clean-core/container/key_value_cache.hh>
#include <shaped-graphics/fwd.hh> // sg::async_compiled_shader
#include <shaped-shader-compiler-dxc/compile_options.hh>
#include <shaped-shader-compiler-dxc/shader_description.hh>

#include <memory>

/// Async, cached DXC compilation. The key is a cc::hash128 over the full compile identity (source,
/// entry point, stage, model, options); the value is a shared async compiled shader. A second compile()
/// for the same key — whether the first is still in flight or already finished — returns the SAME async
/// node, so a shader is never compiled twice.
///
/// Compilation runs on the installed default async pool (cc::install_default_async_pool); with none
/// installed, cc::async_blocking_get drives it inline on the calling thread. Each worker uses its own
/// thread-local ssc::dxc::compiler (the compiler is one-per-thread / not thread-safe).

namespace ssc::dxc
{
class shader_cache
{
public:
    /// Adds a cache tier (see cc::key_value_cache). Front tiers are consulted first.
    void add_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, sg::async_compiled_shader>> provider);

    /// Convenience: append a default in-memory tier holding up to max_entries compiled shaders.
    void add_default_in_memory_provider(cc::isize max_entries = 4096);

    /// The async compiled shader for (desc, options), reusing a cached node if present. Drive with
    /// cc::async_blocking_get(sh) or poll sh->try_value() (yields sg::compiled_shader_handle); on a
    /// compile failure the node carries the DXC diagnostics as an async error.
    ///
    /// `desc.source` must already be preprocessed (compile() rejects #includes) — resolve includes via
    /// ssc::dxc::compiler::preprocess before caching.
    [[nodiscard]] sg::async_compiled_shader compile(shader_description const& desc, compile_options const& options = {});

    /// Runs bookkeeping (e.g. in-memory eviction) on all tiers.
    void apply_bookkeeping();

private:
    [[nodiscard]] cc::hash128 compute_key(shader_description const& desc, compile_options const& options) const;

    cc::key_value_cache<cc::hash128, sg::async_compiled_shader> _cache;
};
} // namespace ssc::dxc
