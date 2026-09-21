#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>

namespace
{
[[nodiscard]] sgl::emit::target target_of(sg::shader_format format)
{
    switch (format)
    {
    case sg::shader_format::dxil:
        return sgl::emit::target::hlsl_dx12;
    case sg::shader_format::spirv:
        return sgl::emit::target::hlsl_vulkan;
    case sg::shader_format::wgsl:
        return sgl::emit::target::wgsl;
    case sg::shader_format::metal_lib:
        return sgl::emit::target::msl;
    default:
        CC_UNREACHABLE("SGL is written for dxil, spirv, wgsl and metal_lib compilers only");
    }
}

class sgl_shader_compiler final : public slib::shader_compiler
{
public:
    explicit sgl_shader_compiler(std::unique_ptr<slib::shader_compiler> inner)
      : _inner(cc::move(inner)), _target(target_of(_inner->target_format()))
    {
    }

    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::sgl; }
    [[nodiscard]] sg::shader_format target_format() const override { return _inner->target_format(); }

    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver resolve) const override
    {
        (void)resolve; // SGL has no include directive

        auto stage = sgl::check::stage::none;
        if (desc.stage == sg::shader_stage::vertex)
            stage = sgl::check::stage::vertex;
        else if (desc.stage == sg::shader_stage::fragment)
            stage = sgl::check::stage::pixel;
        else if (desc.stage == sg::shader_stage::compute)
            stage = sgl::check::stage::compute;
        else
            return cc::error(cc::format("SGL has vertex, pixel and compute entry points only, and '{}' is declared as "
                                        "none of them",
                                        desc.entry_point));

        auto text = sgl::compile_to_text(
            {.source = desc.source,
             .source_name = desc.label.empty() ? cc::string_view("<sgl>") : cc::string_view(desc.label),
             .entry_point = desc.entry_point,
             .stage = stage,
             .target = _target});
        if (text.has_error())
            return cc::error(cc::format("SGL reported errors:\n{}", text.error()));
        // The name the text declares, which is the source's unless this target reserves it.
        auto result = slib::preprocessed_source{.source = cc::move(text.value().text),
                                                .entry_point = cc::move(text.value().entry_point)};
        for (auto& bound : text.value().bound_names)
            result.renamed_bindings.push_back({.reflected = cc::move(bound.emitted), .name = cc::move(bound.host)});
        result.color_targets = text.value().color_targets;
        result.target_struct = cc::move(text.value().target_struct);
        return result;
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        return _inner->compile(desc);
    }

private:
    std::unique_ptr<slib::shader_compiler> _inner;
    sgl::emit::target _target;
};
} // namespace

std::unique_ptr<slib::shader_compiler> slib::create_sgl_compiler(std::unique_ptr<shader_compiler> inner)
{
    CC_ASSERT(inner != nullptr, "an SGL compiler needs the compiler that builds its text");
    return std::make_unique<sgl_shader_compiler>(cc::move(inner));
}
