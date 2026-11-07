#include <clean-core/common/time.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/bench/run.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// What SGL's own SPIR-V costs against SGL's text through DXC, for shaders of real work.
// Both go through SGL's front end, so the difference is the writer against DXC, which a hot reload waits for.

namespace
{
[[nodiscard]] cc::string read_source(cc::string_view relative)
{
    auto adapter = cc::file_read_stream_adapter::open(nx::source_path(relative));
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

struct kernel
{
    cc::string_view name;
    cc::string_view path;
    cc::string_view entry;
};

constexpr kernel k_kernels[] = {
    {"a dispatch", "libs/graphics/shaped-graphics/tests/shaders/sgl/dispatch.sgl", "stamp"},
    {"a-trous", "libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl", "main_cs"},
    {"svgf", "libs/graphics/shaped-rendering/shaders/svgf.sgl", "atrous_cs"},
    {"convolution", "libs/graphics/shaped-rendering/shaders/nn_conv.sgl", "main_cs"},
};

/// The fastest of `runs` calls of `body`: a process lands in one of two speed modes, so a median across runs says little.
template <class Body>
[[nodiscard]] f64 best_of(int runs, Body&& body)
{
    auto best = 1e30;
    for (auto i = 0; i < runs; ++i)
    {
        auto const start = cc::current_time_steady_secs();
        body();
        best = cc::min(best, cc::current_time_steady_secs() - start);
    }
    return best * 1000.0;
}
} // namespace

BENCHMARK("slib - a vertex and pixel pair through SGL's own binaries against DXC, best of N")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr auto path = cc::string_view("examples/graphics/sgl-cube/shaders/cube.sgl");
    auto const source = read_source(path);
    struct stage_entry
    {
        cc::string_view entry;
        sg::shader_stage stage;
    };
    constexpr stage_entry k_pair[] = {{"main_vs", sg::shader_stage::vertex}, {"main_ps", sg::shader_stage::fragment}};

    auto const through_dxc = [&](sgl::emit::target target, ssc::dxc::compile_target binary)
    {
        return best_of(
            60,
            [&]
            {
                for (auto const& s : k_pair)
                {
                    auto text = sgl::compile_to_text(
                        {.source = source, .source_name = path, .entry_point = s.entry, .target = target});
                    REQUIRE(text.has_value());
                    auto built = compiler.value().compile(
                        {.source = text.value().text, .entry_point = text.value().entry_point, .stage = s.stage},
                        {.target = binary, .warnings_as_errors = false, .extra_args = {cc::string("-enable-16bit-types")}});
                    REQUIRE(built.has_value());
                }
            });
    };
    auto const front_only = [&](sgl::emit::target target, sgl::emit::native_route route)
    {
        return best_of(
            400,
            [&]
            {
                for (auto const& s : k_pair)
                {
                    auto written = sgl::compile_to_text(
                        {.source = source, .source_name = path, .entry_point = s.entry, .target = target, .native = route});
                    REQUIRE(written.has_value());
                }
            });
    };
    using route = sgl::emit::native_route;
    for (auto const target : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const is_dx12 = target == sgl::emit::target::hlsl_dx12;
        auto const dxc = through_dxc(target, is_dx12 ? ssc::dxc::compile_target::dxil : ssc::dxc::compile_target::spirv);
        auto const text = front_only(target, route::text);
        auto const native = front_only(target, route::require);
        CC_LOG_INFO("{}: through DXC {} ms, the text alone {} ms, SGL's own {} ms, {}x faster",
                    is_dx12 ? "dxil" : "spirv", dxc, text, native, dxc / native);
    }
}

// EMIT-257: what a line table costs, on the shader with the most lines to say, whose modules reach several files.
BENCHMARK("slib - the viewer's tracer through SGL's own binaries with and without their line table, best of N")
{
    auto names = cc::vector<cc::string>();
    auto sources = cc::vector<cc::string>();
    for (auto const dir : {cc::string_view("libs/graphics/shaped-viewer/shaders/sgl"),
                           cc::string_view("libs/graphics/shaped-rendering/shaders/modules")})
    {
        namespace fs = std::filesystem;
        auto const root = nx::source_path(dir);
        for (auto const& e : fs::directory_iterator(fs::path(root.data(), root.data() + root.size())))
            if (e.is_regular_file() && e.path().extension() == ".sgl")
            {
                names.push_back(cc::string(e.path().generic_string().c_str()));
                sources.push_back(read_source(cc::format("{}/{}", dir, e.path().filename().generic_string().c_str())));
            }
    }
    auto library = cc::vector<sgl::library_file>();
    for (auto i = isize(0); i < names.size(); ++i)
        library.push_back({.name = names[i], .source = sources[i]});

    constexpr auto path = cc::string_view("libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl");
    auto const source = read_source(path);
    for (auto const target : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
    {
        auto const is_dx12 = target == sgl::emit::target::hlsl_dx12;
        auto bytes = isize(0);
        auto const compile = [&](bool line_table)
        {
            return best_of(40,
                           [&]
                           {
                               auto written = sgl::compile_to_text({.source = source,
                                                                    .source_name = path,
                                                                    .library = library,
                                                                    .entry_point = "path_trace",
                                                                    .target = target,
                                                                    .native = sgl::emit::native_route::require,
                                                                    .native_line_table = line_table});
                               REQUIRE(written.has_value());
                               bytes = is_dx12 ? written.value().dxil.size() : written.value().spirv.size() * 4;
                           });
        };
        auto const off = compile(false);
        auto const off_bytes = bytes;
        auto const on = compile(true);
        CC_LOG_INFO("{}: {} ms and {} bytes without the line table, {} ms and {} bytes with it",
                    is_dx12 ? "dxil" : "spirv", off, off_bytes, on, bytes);
    }
}

BENCHMARK("slib - SGL's own DXIL against its text through DXC, one compile")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    for (auto const& k : k_kernels)
    {
        auto const source = read_source(k.path);
        auto const request = sgl::text_request{.source = source,
                                               .source_name = k.path,
                                               .entry_point = k.entry,
                                               .target = sgl::emit::target::hlsl_dx12};
        nx::bench::run(cc::format("{}: DXC", k.name),
                       [&]
                       {
                           auto text = sgl::compile_to_text(request);
                           REQUIRE(text.has_value());
                           auto dxil = compiler.value().compile(
                               {.source = text.value().text, .entry_point = text.value().entry_point},
                               {.target = ssc::dxc::compile_target::dxil,
                                .warnings_as_errors = false,
                                .extra_args = {cc::string("-enable-16bit-types")}});
                           REQUIRE(dxil.has_value());
                       });
        nx::bench::run(cc::format("{}: the text alone", k.name),
                       [&]
                       {
                           auto text = sgl::compile_to_text(request);
                           REQUIRE(text.has_value());
                       });
        auto native = request;
        native.native = sgl::emit::native_route::require;
        nx::bench::run(cc::format("{}: SGL's DXIL", k.name),
                       [&]
                       {
                           auto written = sgl::compile_to_text(native);
                           REQUIRE(written.has_value());
                           REQUIRE(!written.value().dxil.empty());
                       });
    }
}

BENCHMARK("slib - SGL's own SPIR-V against its text through DXC, one compile")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    for (auto const& k : k_kernels)
    {
        auto const source = read_source(k.path);
        auto const request = sgl::text_request{.source = source,
                                               .source_name = k.path,
                                               .entry_point = k.entry,
                                               .target = sgl::emit::target::hlsl_vulkan};
        nx::bench::run(cc::format("{}: DXC", k.name),
                       [&]
                       {
                           auto text = sgl::compile_to_text(request);
                           REQUIRE(text.has_value());
                           auto spirv = compiler.value().compile(
                               {.source = text.value().text, .entry_point = text.value().entry_point},
                               {.target = ssc::dxc::compile_target::spirv,
                                .warnings_as_errors = false,
                                .extra_args = {cc::string("-enable-16bit-types")}});
                           REQUIRE(spirv.has_value());
                       });
        nx::bench::run(cc::format("{}: the text alone", k.name),
                       [&]
                       {
                           auto text = sgl::compile_to_text(request);
                           REQUIRE(text.has_value());
                       });
        auto native = request;
        native.native = sgl::emit::native_route::require;
        nx::bench::run(cc::format("{}: SGL's SPIR-V", k.name),
                       [&]
                       {
                           auto words = sgl::compile_to_text(native);
                           REQUIRE(words.has_value());
                           REQUIRE(!words.value().spirv.empty());
                       });
    }
}
