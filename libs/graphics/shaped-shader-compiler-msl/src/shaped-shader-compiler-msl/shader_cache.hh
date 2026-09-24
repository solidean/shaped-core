#pragma once

#include <blob-cache/keys.hh> // bcache::cache_key, returned by value below
#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/key_value_cache.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/thread/async_backlog.hh>
#include <shaped-graphics/fwd.hh> // sg::async_compiled_shader
#include <shaped-shader-compiler-msl/compile_options.hh>
#include <shaped-shader-compiler-msl/fwd.hh>
#include <shaped-shader-compiler-msl/shader_description.hh>

#include <memory>

/// Async, cached MSL compilation, the metal counterpart of `ssc::dxc::shader_cache`.
/// The key is a cc::hash128 over the full compile identity — source, entry point, stage, workgroup size, every option,
/// the arm that will run and the toolchain's version — and the value is a shared async compiled shader.
/// A second compile() for the same key returns the SAME async node, whether the first is still in flight or finished.
///
/// A compile never runs inside compile(): the node is started on the ambient scheduler, and with none installed
/// cc::async_blocking_get drives it inline on the calling thread.
/// Every compile goes through one `ssc::msl::compiler` for the whole process, resolved once.

class ssc::msl::shader_cache
{
public:
    /// Adds a cache tier (see cc::key_value_cache). Front tiers are consulted first.
    void add_provider(std::shared_ptr<cc::key_value_provider<cc::hash128, sg::async_compiled_shader>> provider);

    /// Convenience: append a default in-memory tier holding up to max_entries compiled shaders.
    void add_default_in_memory_provider(isize max_entries = 4096);

    /// The persistent tier a compile consults: encoded compiled shaders surviving across runs.
    /// Defaults to bcache::default_cache(), opened the first time a compile misses in memory; nullptr turns it off.
    ///
    /// The compile parks on the store, so it needs somewhere to resume: with no ambient scheduler and no worker scope
    /// active, the tier is skipped and the shader is compiled the plain way.
    void set_blob_cache(bcache::blob_cache* cache);

    /// The async compiled shader for (desc, options), reusing a cached node if present.
    /// On a compile failure the node carries the Metal compiler's diagnostics, or this wrapper's, as an async error.
    [[nodiscard]] sg::async_compiled_shader compile(shader_description const& desc, compile_options const& options = {});

    /// Runs bookkeeping (e.g. in-memory eviction) on all tiers.
    void apply_bookkeeping();

    /// Every compile this cache started, which runs to a result whether or not its caller awaits it.
    [[nodiscard]] cc::async_backlog const& backlog() const { return _backlog; }

private:
    [[nodiscard]] cc::hash128 compute_key(shader_description const& desc, compile_options const& options) const;

    /// The persistent-cache key for a compile whose in-memory key is `compile_key`.
    [[nodiscard]] bcache::cache_key persistent_key(cc::hash128 compile_key) const;

    /// The persistent tier, resolved on first use: nullopt means "not chosen yet", a null pointer means OFF.
    /// Lazy so that merely holding a shader_cache never opens a cache file.
    [[nodiscard]] bcache::blob_cache* resolve_blob_cache();

    cc::optional<bcache::blob_cache*> _blob_cache;
    cc::key_value_cache<cc::hash128, sg::async_compiled_shader> _cache;
    cc::async_backlog _backlog = cc::async_backlog("ssc.msl.shader_cache");
};
