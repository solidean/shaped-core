#include <shaped-shader-library/compiler/dxc_compiler.hh>

#if SLIB_HAS_DXC

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-shader-compiler-dxc/compiler.hh>
#include <shaped-shader-compiler-dxc/shader_cache.hh>

namespace
{
/// ssc::dxc::compiler is explicitly one-per-thread, and slib compiles from both the reload watcher's
/// thread and whichever thread acquires — so the instance is thread_local and the seam stays const.
ssc::dxc::compiler* thread_local_compiler()
{
    static thread_local auto compiler = ssc::dxc::compiler::create();
    return compiler.has_value() ? &compiler.value() : nullptr;
}

class dxc_shader_compiler final : public slib::shader_compiler
{
public:
    dxc_shader_compiler() { _cache.add_default_in_memory_provider(); }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::hlsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::dxil; }

    [[nodiscard]] cc::result<cc::string> preprocess(slib::shader_source_description const& desc,
                                                    slib::include_resolver resolve) const override
    {
        auto* const compiler = thread_local_compiler();
        if (compiler == nullptr)
            return cc::error("failed to create the DXC compiler");

        auto result = compiler->preprocess(to_dxc(desc), resolve);
        if (result.has_error())
            return cc::error(cc::move(result.error()));
        return cc::move(result.value().source);
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        // The cache keys on the flattened source and options, so an identical recompile — a reload that
        // touched a file without changing what it expands to — returns the node that already exists.
        return _cache.compile(to_dxc(desc));
    }

private:
    [[nodiscard]] static ssc::dxc::shader_description to_dxc(slib::shader_source_description const& desc)
    {
        return ssc::dxc::shader_description{.source = desc.source, .entry_point = desc.entry_point, .stage = desc.stage};
    }

    // Mutable: compile() is const on the seam (it must be callable from several threads), and the cache
    // is itself thread-safe.
    mutable ssc::dxc::shader_cache _cache;
};
} // namespace

cc::result<std::unique_ptr<slib::shader_compiler>> slib::create_dxc_compiler()
{
    // Fail here rather than on first use, so a broken DXC install surfaces at startup.
    if (thread_local_compiler() == nullptr)
        return cc::error("failed to create the DXC compiler");

    auto compiler = std::make_unique<dxc_shader_compiler>();
    return cc::result<std::unique_ptr<shader_compiler>>(cc::move(compiler));
}

#endif
