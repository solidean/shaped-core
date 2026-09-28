#include <shaped-shader-library/compiler/metal_compiler.hh>

#if SLIB_HAS_METAL

#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-shader-compiler-msl/shader_cache.hh>

namespace
{
class metal_shader_compiler final : public slib::shader_compiler
{
public:
    metal_shader_compiler() { _cache.add_default_in_memory_provider(); }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::metal; }

    /// `metal_lib` is the edge, whichever artifact a compile produced.
    /// A host without the toolchain emits `msl` source instead, and the metal backend accepts both.
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::metal_lib; }

    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver) const override
    {
        // Nothing to flatten: SGL emits text with no `#include`, and a hand-written shader's `#include <metal_stdlib>`
        // is a Clang module the Metal compiler resolves itself.
        return slib::preprocessed_source{.source = desc.source};
    }

    /// The node runs later, on the scheduler, never inside the `acquire` that asked for it.
    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        return _cache.compile({.source = desc.source, .entry_point = desc.entry_point, .stage = desc.stage});
    }

private:
    // Mutable: compile() is const on the seam (it must be callable from several threads), and the cache is itself thread-safe.
    mutable ssc::msl::shader_cache _cache;
};
} // namespace

std::unique_ptr<slib::shader_compiler> slib::create_metal_compiler()
{
    return std::make_unique<metal_shader_compiler>();
}

#else

std::unique_ptr<slib::shader_compiler> slib::create_metal_compiler()
{
    return nullptr;
}

#endif
