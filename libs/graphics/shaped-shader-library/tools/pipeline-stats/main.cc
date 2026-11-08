#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <nexus/args/ambient.hh>
#include <nexus/args/args.hh>
#include <nexus/run.hh>
#include <nexus/test.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/binding/pipeline_layout.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/query/driver_statistic.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// `pipeline-stats compute`: what the vulkan driver says it made of an SGL compute entry point, on SGL's own SPIR-V and
// on DXC's of SGL's text, a line per figure.
// Only a driver's own compiler sees the code a GPU runs; a module's operations are what it was handed.

using namespace cc::primitive_defines;

namespace
{
// 0 = printed; 1 = bad usage, or a file, a compile or a pipeline that failed
constexpr int exit_ok = 0;
constexpr int exit_failed = 1;

[[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    if (file.has_error())
        return {};
    auto stream = file.value().stream();
    auto const bytes = stream.read_all();
    if (bytes.has_error())
        return {};
    return cc::optional<cc::string>(
        cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size())));
}

/// The layout a shader's own bindings state, one group per descriptor set, and an inline block where it states one.
/// SGL states an `@inline` block as the one constants buffer of no set.
[[nodiscard]] sg::pipeline_layout_handle layout_of(sg::context& ctx, sg::compiled_shader const& shader)
{
    auto sets = cc::vector<cc::vector<sg::binding>>();
    auto desc = sg::pipeline_layout_description();
    for (auto const& b : shader.bindings)
    {
        if (!b.group_index.has_value() && b.type == sg::binding_type::constants_buffer)
        {
            desc.inline_constants = b;
            continue;
        }
        auto const set = isize(b.group_index.value_or(0));
        while (sets.size() <= set)
            sets.push_back({});
        sets[set].push_back(b);
    }
    for (auto const& set : sets)
        desc.groups.push_back(ctx.cached.acquire_binding_group_layout(set));
    return ctx.cached.acquire_pipeline_layout(desc);
}

/// One route's pipeline and the driver's figures of it, or why there are none.
[[nodiscard]] cc::result<cc::vector<sg::driver_statistic>> figures_of(sg::context& ctx,
                                                                      slib::shader_library const& lib,
                                                                      cc::string_view source,
                                                                      cc::string_view label,
                                                                      cc::string_view entry,
                                                                      bool native)
{
    auto options = cc::vector<slib::shader_option>();
    // the context prefers DXC's, so the native route is asked for by name
    slib::add_context_options(options, ctx);
    if (native)
        options.push_back({.name = cc::string(slib::k_native_spirv_option), .value = cc::string("true")});
    auto const compiled
        = lib.compile_source(source, sg::shader_stage::compute, entry, sg::shader_format::spirv,
                             {.language = slib::shader_language::sgl, .label = label, .options = options});
    (void)cc::try_async_blocking_get(compiled);
    if (compiled->has_error())
        return cc::error(compiled->try_error()->underlying().to_string());
    auto const& shader = *compiled->try_value();
    auto const pipeline = ctx.cached.acquire_compute_pipeline({.shader = shader, .layout = layout_of(ctx, shader)});
    (void)cc::try_async_blocking_get(pipeline);
    if (pipeline->has_error())
        return cc::error(pipeline->try_error()->underlying().to_string());
    return (*pipeline->try_value())->driver_statistics();
}
/// The same, a layout or a pipeline that throws as its failure.
[[nodiscard]] cc::result<cc::vector<sg::driver_statistic>> guarded(sg::context& ctx,
                                                                   slib::shader_library const& lib,
                                                                   cc::string_view source,
                                                                   cc::string_view label,
                                                                   cc::string_view entry,
                                                                   bool native)
{
    try
    {
        return figures_of(ctx, lib, source, label, entry, native);
    }
    catch (sg::exception const& e)
    {
        return cc::error(cc::string(e.message()));
    }
}
} // namespace

COMMAND("compute")
{
    auto path = cc::string();
    auto entries = cc::vector<cc::string>();
    auto module_dirs = cc::vector<cc::string>();
    auto args
        = nx::args({.name = "pipeline-stats compute",
                    .description = "Prints what the vulkan driver reports of each compute entry point's pipeline, "
                                   "on SGL's own SPIR-V and on DXC's of SGL's text: a line per figure, "
                                   "`entry executable name native dxc`, tab-separated."});
    args.positional("FILE", path, {.desc = "the .sgl source"});
    args.arg({"entry"}, entries, {.desc = "a compute entry point; any number", .metavar = "NAME"});
    args.arg({"module-dir"}, module_dirs,
             {.desc = "a directory of SGL modules the source may use, absolute", .metavar = "DIR"});
    if (auto const r = args.parse(nx::test_args()); r.should_exit())
        return r.exit_code();
    auto const source = read_text(path);
    if (!source.has_value() || entries.empty())
    {
        CC_LOG_ERROR("name a readable .sgl file and at least one entry point");
        return exit_failed;
    }

    auto created = sg::create_vulkan_context({.native_spirv = false, .capture_driver_statistics = true});
    if (created.has_error())
    {
        CC_LOG_ERROR("no vulkan context: {}", created.error().to_string());
        return exit_failed;
    }
    auto& ctx = *created.value();
    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    for (auto i = isize(0); i < module_dirs.size(); ++i)
    {
        auto const mount = cc::format("modules{}", i);
        lib.mount(mount, std::make_shared<slib::real_filesystem>(module_dirs[i]));
        lib.add_module_dir(mount);
    }

    auto failed = false;
    for (auto const& entry : entries)
    {
        auto const native = guarded(ctx, lib, source.value(), path, entry, true);
        auto const dxc = guarded(ctx, lib, source.value(), path, entry, false);
        if (native.has_error() || dxc.has_error())
        {
            CC_LOG_ERROR("{}: {}", entry, native.has_error() ? native.error().to_string() : dxc.error().to_string());
            failed = true;
            continue;
        }
        for (auto const& n : native.value())
        {
            auto other = cc::string("-");
            for (auto const& d : dxc.value())
                if (d.executable == n.executable && d.name == n.name)
                    other = cc::format("{}", d.value);
            cc::println("{}\t{}\t{}\t{}\t{}", entry, n.executable, n.name, n.value, other);
        }
    }
    return failed ? exit_failed : exit_ok;
}

int main(int argc, char** argv)
{
    return nx::run(argc, argv);
}
