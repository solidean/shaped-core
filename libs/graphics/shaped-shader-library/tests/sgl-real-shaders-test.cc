#include <clean-core/algorithm/sort.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <cstdlib>
#include <filesystem>

using namespace cc::primitive_defines;

// The repo's largest shaders through SGL's own binaries: what a viewer and a traced drawing compile, modules and all.
// The fixtures' sweep reads a file alone, so what `use`s a module of another directory is this test's.

namespace
{
[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto file = cc::file_read_stream_adapter::open(path);
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

[[nodiscard]] cc::vector<cc::string> files_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    auto ec = std::error_code();
    for (auto const& e : fs::directory_iterator(fs::path(root.data(), root.data() + root.size()), ec))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(cc::string(e.path().generic_string().c_str()));
    cc::sort(found);
    return found;
}

void write_words(cc::string_view path, cc::span<u32 const> words)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    REQUIRE(stream.write(words.as_bytes()).has_value());
    REQUIRE(stream.flush().has_value());
}

/// The module directories these shaders `use`, read: names and sources stay put, since `files` views them.
struct module_library
{
    cc::vector<cc::string> names;
    cc::vector<cc::string> sources;
    cc::vector<sgl::library_file> files;

    void read()
    {
        for (auto const& dir : {nx::source_path("libs/graphics/shaped-viewer/shaders/sgl"),
                                nx::source_path("libs/graphics/shaped-rendering/shaders/modules")})
            for (auto const& file : files_under(dir))
            {
                names.push_back(file);
                sources.push_back(read_file(file));
            }
        for (auto i = isize(0); i < names.size(); ++i)
            files.push_back({.name = names[i], .source = sources[i]});
    }
};

[[nodiscard]] cc::vector<cc::string> real_shaders()
{
    auto files = files_under(nx::source_path("libs/graphics/shaped-viewer/shaders"));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-rendering/tests/shaders")));
    files.push_back_range(files_under(nx::source_path("examples/graphics/slug-traced/shaders")));
    return files;
}
} // namespace

TEST("slib - the viewer's tracer and the traced drawings lower on both writers, each past its validator")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const has_spirv_val = std::system("spirv-val --version > NUL 2>&1") == 0;
    auto const module_path
        = cc::format("{}/sgl-real-shaders-test.spv", std::filesystem::temp_directory_path().generic_string().c_str());
    auto const command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
    auto command_text = cc::vector<char>::create_copy_of(cc::span<char const>(command.data(), command.size()));
    command_text.push_back('\0');

    auto library = module_library();
    library.read();
    auto dxil = 0;
    auto spirv = 0;
    auto is_tracer_native = false;
    auto is_slug_native = false;
    for (auto const& file : real_shaders())
    {
        auto const source = read_file(file);
        auto const all = sgl::compile_all_to_text(
            {.source = source, .source_name = file, .library = library.files, .targets = {sgl::emit::target::hlsl_dx12}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            auto const compile = [&](sgl::emit::target target)
            {
                return sgl::compile_to_text(
                    {.source = source,
                     .source_name = file,
                     .library = library.files,
                     .entry_point = entry.entry_point,
                     .target = target,
                     .diagnostics = true,
                     .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                     .native = sgl::emit::native_route::prefer});
            };
            auto const on_dx12 = compile(sgl::emit::target::hlsl_dx12);
            if (on_dx12.has_value() && !on_dx12.value().dxil.empty())
            {
                ++dxil;
                is_tracer_native = is_tracer_native || entry.entry_point == "path_trace";
                is_slug_native = is_slug_native || entry.entry_point == "cut_surface";
                auto const refused = compiler.value().validate(on_dx12.value().dxil);
                CHECK(!refused.has_value())
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("validator", refused.value_or(cc::string()));
            }
            else if (on_dx12.has_value())
                CC_LOG_INFO("{}: {}: {}", file, entry.entry_point, on_dx12.value().native_gap);

            auto const on_vulkan = compile(sgl::emit::target::hlsl_vulkan);
            if (!on_vulkan.has_value() || on_vulkan.value().spirv.empty())
            {
                if (on_vulkan.has_value())
                    CC_LOG_INFO("{}: {}: {}", file, entry.entry_point, on_vulkan.value().native_gap);
                continue;
            }
            ++spirv;
            if (has_spirv_val)
            {
                write_words(module_path, on_vulkan.value().spirv);
                CHECK(std::system(command_text.data()) == 0).dump("file", file).dump("entry", entry.entry_point);
            }
        }
    }
    CC_LOG_INFO("the viewer's and the drawings' shaders: {} through DXIL, {} through SPIR-V", dxil, spirv);
    CHECK(is_tracer_native);
    CHECK(is_slug_native);
}
