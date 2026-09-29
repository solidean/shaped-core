#include "compiler.hh"

#include "impl/metal_driver.hh"
#include "impl/msl_reflection.hh"

#include <clean-core/container/pinned_data.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>

namespace ssc::msl
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "ssc.msl");
}

namespace
{
using namespace cc::primitive_defines;

[[nodiscard]] cc::string_view optimization_flag(ssc::msl::optimization_level level)
{
    switch (level)
    {
    case ssc::msl::optimization_level::disabled:
        return "-O0";
    case ssc::msl::optimization_level::level_1:
        return "-O1";
    case ssc::msl::optimization_level::level_2:
        return "-O2";
    case ssc::msl::optimization_level::level_3:
        return "-O3";
    }
    return "-O2";
}

/// The command line for one compile, reading stdin and writing the metallib to stdout.
[[nodiscard]] cc::vector<cc::string> driver_arguments(ssc::msl::compile_options const& options)
{
    auto args = cc::vector<cc::string>();
    args.push_back("-x");
    args.push_back("metal");
    args.push_back(cc::string(optimization_flag(options.optimization)));

    if (!options.language_version.empty())
        args.push_back(cc::format("-std={}", options.language_version));
    if (options.debug_info)
        args.push_back("-frecord-sources");
    if (options.warnings_as_errors)
        args.push_back("-Werror");

    for (auto const& define : options.defines)
        args.push_back(cc::format("-D{}", define));
    for (auto const& extra : options.extra_args)
        args.push_back(extra);

    // Output to stdout, input from stdin, so a compile touches no filesystem at all.
    args.push_back("-o");
    args.push_back("-");
    args.push_back("-");
    return args;
}

/// The text the source arm hands the driver: `source`, behind one `#define` per entry of `options.defines`.
/// The driver compiles it with default options, so a flag only the Metal compiler takes is an error rather than lost.
[[nodiscard]] cc::result<cc::string> source_with_defines(cc::string_view source, ssc::msl::compile_options const& options)
{
    if (!options.language_version.empty() || !options.extra_args.empty() || options.warnings_as_errors)
        return cc::error("compile: language_version, extra_args and warnings_as_errors need the metallib arm, since "
                         "the driver compiles MSL source with its default options");

    if (options.defines.empty())
        return cc::string(source);

    auto text = cc::string();
    for (auto const& define : options.defines)
    {
        auto const view = cc::string_view(define);
        auto const equals = view.find('=');
        if (equals < 0)
            text += cc::format("#define {}\n", view);
        else
            text += cc::format("#define {} {}\n", view.subview({.start = 0, .end = equals}),
                               view.subview({.start = equals + 1, .end = view.size()}));
    }
    // So a line the driver reports is a line of the source as written.
    text += "#line 1\n";
    text += source;
    return text;
}

/// The provenance a cache key folds in: what compiled it, which version, and the flags that shaped the result.
[[nodiscard]] cc::string signature_of(ssc::msl::compile_options const& options, cc::string_view arm)
{
    auto signature = cc::string(arm);
    for (auto const& arg : driver_arguments(options))
    {
        signature += " ";
        signature += arg;
    }
    return signature;
}
} // namespace

struct ssc::msl::compiler::state
{
    toolchain_info toolchain;
};

ssc::msl::compiler::compiler(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}
ssc::msl::compiler::compiler(compiler&&) noexcept = default;
ssc::msl::compiler& ssc::msl::compiler::operator=(compiler&&) noexcept = default;
ssc::msl::compiler::~compiler() = default;

cc::result<ssc::msl::compiler> ssc::msl::compiler::create()
{
    auto s = cc::make_unique<state>();
    s->toolchain.driver_path = impl::resolve_metal_driver();
    s->toolchain.is_available = !s->toolchain.driver_path.empty();
    if (s->toolchain.is_available)
        s->toolchain.version = impl::query_driver_version(s->toolchain.driver_path);
    else
        CC_LOG_INFO("no Metal toolchain on this host, so compiles emit MSL source rather than a metallib");

    return compiler(cc::move(s));
}

ssc::msl::toolchain_info const& ssc::msl::compiler::toolchain() const
{
    return _state->toolchain;
}

cc::result<sg::compiled_shader> ssc::msl::compiler::compile(shader_description const& desc,
                                                            compile_options const& options) const
{
    if (desc.source.empty())
        return cc::error("compile: the shader description carries no source");

    // Reflection reads the text, so it runs whichever arm produces the blob — and it is what refuses a stage metal
    // has no shape for, before a compiler is ever spawned.
    auto reflected = impl::reflect(desc.source, desc.entry_point, desc.stage);
    if (reflected.has_error())
        return cc::error(cc::move(reflected).error());

    auto const wants_metallib = options.artifact == artifact_kind::metallib
                             || (options.artifact == artifact_kind::automatic && _state->toolchain.is_available);

    if (options.artifact == artifact_kind::metallib && !_state->toolchain.is_available)
        return cc::error("compile: a metallib was asked for and this host has no Metal toolchain — install it with "
                         "`xcodebuild -downloadComponent MetalToolchain`, or ask for msl_source");

    auto shader = sg::compiled_shader();
    shader.stage = desc.stage;
    shader.entry_point = desc.entry_point;
    shader.bindings = cc::move(reflected.value().bindings);
    shader.workgroup_size = desc.workgroup_size.has_value() ? desc.workgroup_size : reflected.value().workgroup_size;

    if (wants_metallib)
    {
        auto run = impl::run_process(_state->toolchain.driver_path, driver_arguments(options), desc.source);
        if (run.has_error())
            return cc::error(cc::move(run).error());

        if (run.value().exit_code != 0)
            return cc::error(
                cc::format("compile: the Metal compiler rejected '{}':\n{}", desc.entry_point, run.value().err));

        if (run.value().out.empty())
            return cc::error(cc::format("compile: the Metal compiler produced no metallib for '{}'", desc.entry_point));

        shader.format = sg::shader_format::metal_lib;
        shader.bytecode = cc::make_pinned_data(cc::move(run.value().out));
    }
    else
    {
        auto text = source_with_defines(desc.source, options);
        if (text.has_error())
            return cc::error(cc::move(text).error());

        // The blob is the source itself, which the driver compiles when a pipeline is built.
        auto const& source = text.value();
        auto bytes = cc::vector<byte>::create_uninitialized(source.size());
        for (auto i = isize(0); i < source.size(); ++i)
            bytes[i] = byte(source[i]);

        shader.format = sg::shader_format::msl;
        shader.bytecode = cc::make_pinned_data(cc::move(bytes));
    }

    shader.compiler = {.name = "metal",
                       .version = _state->toolchain.version,
                       .signature = signature_of(options, wants_metallib ? "metallib" : "msl")};

    return shader;
}
