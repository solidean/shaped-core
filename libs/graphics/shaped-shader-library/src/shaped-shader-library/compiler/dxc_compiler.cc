#include <shaped-shader-library/compiler/dxc_compiler.hh>

#if SLIB_HAS_DXC

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-shader-compiler-dxc/compile_options.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>
#include <shaped-shader-compiler-dxc/shader_cache.hh>

namespace
{
/// ssc::dxc::compiler is explicitly one-per-thread, and slib compiles from both the reload watcher's thread and whichever thread acquires.
/// So the instance is thread_local, and the seam stays const.
ssc::dxc::compiler* thread_local_compiler()
{
    static thread_local auto compiler = ssc::dxc::compiler::create();
    return compiler.has_value() ? &compiler.value() : nullptr;
}

class dxc_shader_compiler final : public slib::shader_compiler
{
public:
    explicit dxc_shader_compiler(ssc::dxc::compile_target target) : _target(target)
    {
        _cache.add_default_in_memory_provider();
    }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::hlsl; }

    [[nodiscard]] sg::shader_format target_format() const override
    {
        return _target == ssc::dxc::compile_target::spirv ? sg::shader_format::spirv : sg::shader_format::dxil;
    }

    [[nodiscard]] cc::result<cc::string> preprocess(slib::shader_source_description const& desc,
                                                    slib::include_resolver resolve) const override
    {
        auto* const compiler = thread_local_compiler();
        if (compiler == nullptr)
            return cc::error("failed to create the DXC compiler");

        // The target goes in here as well as at compile: it is what defines `__spirv__`, which is how one source
        // writes both a DXIL and a SPIR-V spelling of the same binding.
        auto result = compiler->preprocess(to_dxc(desc), resolve, {.target = _target});
        if (result.has_error())
            return cc::error(cc::move(result.error()));
        return cc::move(result.value().source);
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        // The cache keys on the flattened source and options: a reload that touched a file without changing what it expands to returns the node that already exists.
        return _cache.compile(to_dxc(desc), {.target = _target});
    }

private:
    [[nodiscard]] static ssc::dxc::shader_description to_dxc(slib::shader_source_description const& desc)
    {
        return ssc::dxc::shader_description{.source = desc.source, .entry_point = desc.entry_point, .stage = desc.stage};
    }

    ssc::dxc::compile_target _target;

    // Mutable: compile() is const on the seam (it must be callable from several threads), and the cache is itself thread-safe.
    // One cache serves both targets safely: compute_key folds the compile options in, so a dxil and a spirv build of
    // the same source are different entries rather than a collision.
    mutable ssc::dxc::shader_cache _cache;
};
} // namespace

namespace
{
cc::result<std::unique_ptr<slib::shader_compiler>> create_for(ssc::dxc::compile_target target)
{
    // Fail here rather than on first use, so a broken DXC install surfaces at startup.
    if (thread_local_compiler() == nullptr)
        return cc::error("failed to create the DXC compiler");

    auto compiler = std::make_unique<dxc_shader_compiler>(target);
    return cc::result<std::unique_ptr<slib::shader_compiler>>(cc::move(compiler));
}
} // namespace

cc::result<std::unique_ptr<slib::shader_compiler>> slib::create_dxc_compiler()
{
    return create_for(ssc::dxc::compile_target::dxil);
}

cc::result<std::unique_ptr<slib::shader_compiler>> slib::create_dxc_spirv_compiler()
{
    return create_for(ssc::dxc::compile_target::spirv);
}

#endif
