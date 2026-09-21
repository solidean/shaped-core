#include <clean-core/container/pinned_data.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/binding/wgsl_declarations.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>

using namespace cc::primitive_defines;

namespace
{
sg::async_compiled_shader make_failed_shader(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
}

class wgsl_shader_compiler final : public slib::shader_compiler
{
public:
    [[nodiscard]] slib::shader_language source_language() const override { return slib::shader_language::wgsl; }
    [[nodiscard]] sg::shader_format target_format() const override { return sg::shader_format::wgsl; }

    [[nodiscard]] cc::result<slib::preprocessed_source> preprocess(slib::shader_source_description const& desc,
                                                                   slib::include_resolver resolve) const override
    {
        // WGSL has no include directive, so there is nothing to flatten.
        (void)resolve;
        return slib::preprocessed_source{.source = cc::string(desc.source)};
    }

    [[nodiscard]] sg::async_compiled_shader compile(slib::shader_source_description const& desc) const override
    {
        auto declared = slib::parse_wgsl_declarations(desc.source);
        if (declared.has_error())
            return make_failed_shader(declared.error().to_string());

        auto& d = declared.value();
        if (d.stage != desc.stage)
            return make_failed_shader(cc::format(
                "the module's entry point '{}' is not the stage the package declares for it", d.entry_point));
        if (d.entry_point != desc.entry_point)
            return make_failed_shader(
                cc::format("the module's entry point is '{}', not '{}'", d.entry_point, desc.entry_point));

        auto bytes = cc::vector<byte>::create_uninitialized(desc.source.size());
        for (auto i = isize(0); i < desc.source.size(); ++i)
            bytes[i] = byte(desc.source[i]);

        return cc::make_async_from_value(sg::compiled_shader{
            .stage = d.stage,
            .format = sg::shader_format::wgsl,
            .entry_point = cc::move(d.entry_point),
            .bytecode = cc::make_pinned_data(cc::move(bytes)),
            .bindings = cc::move(d.bindings),
            .workgroup_size = d.workgroup_size,
            .compiler = {.name = cc::string("slib-wgsl"), .version = cc::string("1"), .signature = cc::string()},
        });
    }
};
} // namespace

std::unique_ptr<slib::shader_compiler> slib::create_wgsl_compiler()
{
    return std::make_unique<wgsl_shader_compiler>();
}
