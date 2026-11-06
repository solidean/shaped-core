#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/bench/run.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

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
} // namespace

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
