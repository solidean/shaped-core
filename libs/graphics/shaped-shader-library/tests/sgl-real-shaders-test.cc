#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <cstdlib>
#include <filesystem>

using namespace cc::primitive_defines;

// The repo's largest shaders through SGL's own binaries: what a viewer and a traced drawing compile, modules and all.
// The fixtures' sweep reads a file alone, so what `use`s a module of another directory is this test's.

namespace
{
#ifdef CC_OS_WINDOWS
constexpr bool k_has_validator = true;
#else
constexpr bool k_has_validator = false;
#endif

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
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
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
            auto const compile = [&](sgl::emit::target target, bool line_table = false)
            {
                return sgl::compile_to_text(
                    {.source = source,
                     .source_name = file,
                     .library = library.files,
                     .entry_point = entry.entry_point,
                     .target = target,
                     .diagnostics = true,
                     .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                     .native = sgl::emit::native_route::prefer,
                     .native_line_table = line_table});
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
                // EMIT-257: with its line table too, whose lines a module directory's files reach
                auto const lined = compile(sgl::emit::target::hlsl_dx12, true);
                REQUIRE(lined.has_value());
                auto const lined_refused = compiler.value().validate(lined.value().dxil);
                CHECK(!lined_refused.has_value())
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("validator", lined_refused.value_or(cc::string()));
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
                auto const lined = compile(sgl::emit::target::hlsl_vulkan, true);
                REQUIRE(lined.has_value());
                write_words(module_path, lined.value().spirv);
                CHECK(std::system(command_text.data()) == 0)
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("with", "its line table");
            }
        }
    }
    CC_LOG_INFO("the viewer's and the drawings' shaders: {} through DXIL, {} through SPIR-V", dxil, spirv);
    CHECK(is_tracer_native);
    CHECK(is_slug_native);
}

// A declaration the passes leave uncalled is dropped as the module is written, and the writer's own values of the
// operations it still calls stay right: the environment's pixel stage reads a store_output declared after one such.
TEST("slib - SGL's DXIL marks every target the viewer's environment writes, past a declaration the passes dropped")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's disassembler reads the container, and only Windows has it");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto library = module_library();
    library.read();
    auto const file = nx::source_path("libs/graphics/shaped-viewer/shaders/raster_pass.sgl");
    auto const source = read_file(file);
    auto const written = sgl::compile_to_text({.source = source,
                                               .source_name = file,
                                               .library = library.files,
                                               .entry_point = "environment_ps",
                                               .target = sgl::emit::target::hlsl_dx12,
                                               .native = sgl::emit::native_route::require});
    REQUIRE(written.has_value());
    REQUIRE(!written.value().dxil.empty());
    auto const text = compiler.value().disassemble(written.value().dxil);
    REQUIRE(text.has_value());

    // the output signature's rows, `; SV_Target <index> <mask> <register> TARGET float <used>`, before the PSV's
    auto const all = cc::string_view(text.value());
    auto const signature_end = all.size();
    auto targets = 0;
    auto unused = 0;
    auto start = isize(0);
    for (auto i = isize(0); i <= signature_end; ++i)
    {
        if (i < signature_end && all[i] != '\n')
            continue;
        auto line = all.subview({.offset = start, .size = i - start});
        start = i + 1;
        while (!line.empty() && (line[line.size() - 1] == ' ' || line[line.size() - 1] == '\r'))
            line = line.subview({.offset = 0, .size = line.size() - 1});
        if (!line.starts_with("; SV_Target") || !line.contains("TARGET"))
            continue;
        ++targets;
        unused += line.ends_with("float") ? 1 : 0;
    }
    CHECK(targets == 8);
    CHECK(unused == 0).dump("disassembly", text.value());
}

// EMIT-184: on a device with micromaps every query takes `allocateRayQuery2` at Shader Model 6.9, and SPIR-V states the
// capability DXC's own text leaves out.
// A device with them cannot be asked for here, so this holds what no draw on such a device has.
TEST("slib - a query on a device with opacity micromaps says so on both routes, past both validators")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source
        = "require ray_query\n"
          "\n"
          "binding scene:\n"
          "    world: acceleration_structure[.triangles]\n"
          "    hits: mut buffer[float4]\n"
          "\n"
          "@compute(1) fun traced(@thread_id id: int3){scene}:\n"
          "    let r = ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0))\n"
          "    let h = scene.world.trace(r, flags = ray_flags.force_omm_2_state)\n"
          "    scene.hits[id.x] = float4(h.t, 0.0, 0.0, 0.0)\n";
    auto const features = sgl::check::feature_set(sgl::check::feature::opacity_micromap);
    auto const on_dx12 = sgl::compile_to_text({.source = source,
                                               .entry_point = "traced",
                                               .target = sgl::emit::target::hlsl_dx12,
                                               .device_features = features,
                                               .native = sgl::emit::native_route::require});
    REQUIRE(on_dx12.has_value());
    auto const refused = compiler.value().validate(on_dx12.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
    auto const text = compiler.value().disassemble(on_dx12.value().dxil);
    REQUIRE(text.has_value());
    CHECK(text.value().contains("@dx.op.allocateRayQuery2(i32 258, i32 1536, i32 1)"));
    CHECK(text.value().contains("!\"cs\", i32 6, i32 9"));

    auto const on_vulkan = sgl::compile_to_text({.source = source,
                                                 .entry_point = "traced",
                                                 .target = sgl::emit::target::hlsl_vulkan,
                                                 .device_features = features,
                                                 .native = sgl::emit::native_route::require});
    REQUIRE(on_vulkan.has_value());
    auto const& words = on_vulkan.value().spirv;
    auto const states = [&](u32 opcode, u32 operand)
    {
        for (auto at = isize(5); at < words.size(); at += words[at] >> 16)
            if ((words[at] & 0xffffu) == opcode && (words[at] >> 16) >= 2 && words[at + 1] == operand)
                return true;
        return false;
    };
    constexpr u32 k_op_capability = 17;
    constexpr u32 k_opacity_micromap = 5381;
    CHECK(states(k_op_capability, k_opacity_micromap));
    if (std::system("spirv-val --version > NUL 2>&1") != 0)
        return;
    auto const module_path
        = cc::format("{}/sgl-micromap-test.spv", std::filesystem::temp_directory_path().generic_string().c_str());
    write_words(module_path, words);
    auto const command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
    auto command_text = cc::vector<char>::create_copy_of(cc::span<char const>(command.data(), command.size()));
    command_text.push_back('\0');
    CHECK(std::system(command_text.data()) == 0);
}

// A required-native package hard-errors on a gap, and a feature the test machine lacks hides one.
// So every fixture is compiled as a device with these would, and what falls to a gap is named here to be seen.
TEST("slib - a device's micromaps and reordering leave only the named gaps on either route")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const features = sgl::check::feature_set(sgl::check::feature::opacity_micromap)
                        | sgl::check::feature::shader_execution_reordering | sgl::check::feature::vertex_pipeline_stores;
    auto library = module_library();
    library.read();
    auto files = files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl_mixed")));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    files.push_back_range(real_shaders());
    auto const has_spirv_val = std::system("spirv-val --version > NUL 2>&1") == 0;
    auto const module_path
        = cc::format("{}/sgl-device-features-test.spv", std::filesystem::temp_directory_path().generic_string().c_str());
    auto const command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
    auto command_text = cc::vector<char>::create_copy_of(cc::span<char const>(command.data(), command.size()));
    command_text.push_back('\0');
    auto refused_count = 0;
    auto lowered = 0;
    for (auto const& file : files)
    {
        auto const source = read_file(file);
        auto const all = sgl::compile_all_to_text(
            {.source = source, .source_name = file, .library = library.files, .targets = {sgl::emit::target::hlsl_dx12}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
            for (auto const target : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
            {
                auto const e = sgl::compile_to_text({.source = source,
                                                     .source_name = file,
                                                     .library = library.files,
                                                     .entry_point = entry.entry_point,
                                                     .target = target,
                                                     .diagnostics = true,
                                                     .device_features = features,
                                                     .native = sgl::emit::native_route::prefer});
                if (!e.has_value())
                    continue;
                if (!e.value().native_gap.empty())
                {
                    // what a ray-tracing stage leaves is metal's own, so a required-native package meets no other
                    auto const is_metals = e.value().native_gap.contains("only metal");
                    CHECK((e.value().stage < sgl::check::stage::raygen || is_metals))
                        .dump("entry", entry.entry_point)
                        .dump("gap", e.value().native_gap);
                    CC_LOG_INFO("device gap {}: {}", target == sgl::emit::target::hlsl_dx12 ? "dxil" : "spirv",
                                e.value().native_gap);
                    continue;
                }
                ++lowered;
                if (target != sgl::emit::target::hlsl_dx12)
                {
                    // a process per module, so a default run validates every eighth and --thorough takes them all
                    if (has_spirv_val && (nx::is_thorough() || lowered % 8 == 0))
                    {
                        write_words(module_path, e.value().spirv);
                        CHECK(std::system(command_text.data()) == 0).dump("file", file).dump("entry", entry.entry_point);
                    }
                    continue;
                }
                auto const refused = compiler.value().validate(e.value().dxil);
                refused_count += refused.has_value() ? 1 : 0;
                CHECK(!refused.has_value())
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("validator", refused.value_or(cc::string()));
            }
    }
    CC_LOG_INFO("device features: {} lowered, {} refused", lowered, refused_count);
    CHECK(lowered >= 690);
}
