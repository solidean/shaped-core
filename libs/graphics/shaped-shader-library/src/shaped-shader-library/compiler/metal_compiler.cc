#include <shaped-shader-library/compiler/metal_compiler.hh>

#if SLIB_HAS_METAL

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-shader-compiler-msl/compiler.hh>

namespace
{
sg::async_compiled_shader make_failed_shader(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
}

/// One compiler per thread, as slib compiles from the reload watcher and from whichever thread acquires.
/// `ssc::msl::compiler` resolves the toolchain once in `create`, so sharing one per thread also shares that lookup.
ssc::msl::compiler* thread_local_compiler()
{
    static thread_local auto compiler = ssc::msl::compiler::create();
    return compiler.has_value() ? &compiler.value() : nullptr;
}

class metal_shader_compiler final : public slib::shader_compiler
{
public:
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

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        auto* const compiler = thread_local_compiler();
        if (compiler == nullptr)
            return make_failed_shader("failed to create the metal shader compiler");

        auto shader = compiler->compile({.source = desc.source, .entry_point = desc.entry_point, .stage = desc.stage});
        if (shader.has_error())
            return make_failed_shader(shader.error().to_string());

        return cc::make_async_from_value(cc::move(shader.value()));
    }
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
