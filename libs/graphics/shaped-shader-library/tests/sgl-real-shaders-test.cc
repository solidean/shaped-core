#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <nexus/async-test.hh>
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

/// SPIR-V modules spirv-val reads in one process: each is a file in a directory of this process's own.
/// A process per module was most of these tests' time, and on a loaded machine long enough for the watchdog.
/// A refused batch is validated again module by module, so a failure still names its file and entry point.
class spirv_batch
{
public:
    spirv_batch() : _dir(cc::temp_file_path("sgl-spirv-batch"))
    {
        std::filesystem::create_directories(_dir.c_str_materialize());
    }
    spirv_batch(spirv_batch const&) = delete;
    spirv_batch& operator=(spirv_batch const&) = delete;
    ~spirv_batch()
    {
        auto ec = std::error_code();
        std::filesystem::remove_all(_dir.c_str_materialize(), ec);
    }

    void add(cc::span<u32 const> words, cc::string_view file, cc::string_view entry_point, cc::string_view with = "")
    {
        auto const path = cc::format("{}/{}.spv", _dir, _modules.size());
        write_words(path, words);
        _modules.push_back(
            {.path = path, .file = cc::string(file), .entry_point = cc::string(entry_point), .with = cc::string(with)});
    }

    [[nodiscard]] isize size() const { return _modules.size(); }

    void validate() const
    {
        if (_modules.empty() || run(_dir))
            return;
        for (auto const& m : _modules)
            CHECK(run(m.path)).dump("file", m.file).dump("entry", m.entry_point).dump("with", m.with);
    }

private:
    struct module
    {
        cc::string path;
        cc::string file;
        cc::string entry_point;
        cc::string with;
    };

    [[nodiscard]] static bool run(cc::string_view path)
    {
        auto command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", path);
        return std::system(command.c_str_materialize()) == 0;
    }

    cc::string _dir;
    cc::vector<module> _modules;
};

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

// A task per entry point, since each compiles five times through SGL and that one after another was the binary's critical path.
ASYNC_TEST("slib - the viewer's tracer and the traced drawings lower on both writers, each past its validator")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
    if (ssc::dxc::compiler::create().has_error())
        SKIP("no DXC");
    auto const has_spirv_val = std::system("spirv-val --version > NUL 2>&1") == 0;

    auto library = module_library();
    library.read();
    auto const files = real_shaders();
    auto sources = cc::vector<cc::string>();
    for (auto const& file : files)
        sources.push_back(read_file(file));

    auto entries = cc::vector<cc::vector<cc::string>>::create_defaulted(files.size());
    co_await cc::async_parallel_for(files.size(),
                                    [&](isize f)
                                    {
                                        auto const all
                                            = sgl::compile_all_to_text({.source = sources[f],
                                                                        .source_name = files[f],
                                                                        .library = library.files,
                                                                        .targets = {sgl::emit::target::hlsl_dx12}});
                                        if (all.has_value())
                                            for (auto const& entry : all.value())
                                                entries[f].push_back(entry.entry_point);
                                    },
                                    {.grain = 1});

    struct job
    {
        isize file = 0;
        cc::string entry_point;
        bool dxil = false;
        bool spirv = false;
        cc::vector<u32> plain_spirv;
        cc::vector<u32> lined_spirv;
    };
    auto jobs = cc::vector<job>();
    for (auto f = isize(0); f < files.size(); ++f)
        for (auto const& entry_point : entries[f])
            jobs.push_back({.file = f, .entry_point = entry_point});

    co_await cc::async_parallel_for(
        jobs.size(),
        [&](isize j)
        {
            auto& job = jobs[j];
            auto const& file = files[job.file];
            auto const compile = [&](sgl::emit::target target, bool line_table = false)
            {
                return sgl::compile_to_text(
                    {.source = sources[job.file],
                     .source_name = file,
                     .library = library.files,
                     .entry_point = job.entry_point,
                     .target = target,
                     .diagnostics = true,
                     .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                     .native = sgl::emit::native_route::prefer,
                     .native_debug_info = line_table ? sgl::emit::debug_info::lines : sgl::emit::debug_info::none});
            };
            auto const on_dx12 = compile(sgl::emit::target::hlsl_dx12);
            if (on_dx12.has_value() && !on_dx12.value().dxil.empty())
            {
                job.dxil = true;
                // a compiler is not thread-safe, so each task takes its own
                auto compiler = ssc::dxc::compiler::create();
                REQUIRE(compiler.has_value());
                auto const refused = compiler.value().validate(on_dx12.value().dxil);
                CHECK(!refused.has_value())
                    .dump("file", file)
                    .dump("entry", job.entry_point)
                    .dump("validator", refused.value_or(cc::string()));
                // EMIT-257: with its line table too, whose lines a module directory's files reach
                auto const lined = compile(sgl::emit::target::hlsl_dx12, true);
                REQUIRE(lined.has_value());
                auto const lined_refused = compiler.value().validate(lined.value().dxil);
                CHECK(!lined_refused.has_value())
                    .dump("file", file)
                    .dump("entry", job.entry_point)
                    .dump("validator", lined_refused.value_or(cc::string()));
            }
            else if (on_dx12.has_value())
                CC_LOG_INFO("{}: {}: {}", file, job.entry_point, on_dx12.value().native_gap);

            auto const on_vulkan = compile(sgl::emit::target::hlsl_vulkan);
            if (!on_vulkan.has_value() || on_vulkan.value().spirv.empty())
            {
                if (on_vulkan.has_value())
                    CC_LOG_INFO("{}: {}: {}", file, job.entry_point, on_vulkan.value().native_gap);
                return;
            }
            job.spirv = true;
            if (has_spirv_val)
            {
                job.plain_spirv = cc::vector<u32>::create_copy_of(on_vulkan.value().spirv);
                auto const lined = compile(sgl::emit::target::hlsl_vulkan, true);
                REQUIRE(lined.has_value());
                job.lined_spirv = cc::vector<u32>::create_copy_of(lined.value().spirv);
            }
        },
        {.grain = 1});

    auto spirv_modules = spirv_batch();
    auto dxil = 0;
    auto spirv = 0;
    auto is_tracer_native = false;
    auto is_slug_native = false;
    for (auto const& job : jobs)
    {
        dxil += job.dxil ? 1 : 0;
        spirv += job.spirv ? 1 : 0;
        is_tracer_native = is_tracer_native || (job.dxil && job.entry_point == "path_trace");
        is_slug_native = is_slug_native || (job.dxil && job.entry_point == "cut_surface");
        if (!job.plain_spirv.empty())
            spirv_modules.add(job.plain_spirv, files[job.file], job.entry_point);
        if (!job.lined_spirv.empty())
            spirv_modules.add(job.lined_spirv, files[job.file], job.entry_point, "its line table");
    }
    spirv_modules.validate();
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
    auto spirv_modules = spirv_batch();
    spirv_modules.add(words, "a query on a micromapped device", "main");
    spirv_modules.validate();
}

// EMIT-275, EMIT-277: the candidate loop's native form on both writers, each past its validator, from the corpus's
// queries: a procedural one that breaks, a triangle one that ends its search at a commit and returns out of its loop
// through a helper, and a mixed one with a query in its body.
TEST("slib - a candidate loop lowers on both writers, each past its validator")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const source
        = read_file(nx::source_path("libs/graphics/shaped-graphics-language/tests/corpus/types/queries.sgl"));
    auto const has_spirv_val = std::system("spirv-val --version > NUL 2>&1") == 0;
    auto spirv_modules = spirv_batch();
    for (auto const entry : {"gather_decals", "trace_cells", "trace_mixed"})
    {
        auto const on_dx12 = sgl::compile_to_text({.source = source,
                                                   .entry_point = entry,
                                                   .target = sgl::emit::target::hlsl_dx12,
                                                   .native = sgl::emit::native_route::require});
        REQUIRE(on_dx12.has_value()).dump("entry", entry);
        auto const refused = compiler.value().validate(on_dx12.value().dxil);
        CHECK(!refused.has_value()).dump("entry", entry).dump("validator", refused.value_or(cc::string()));
        auto const text = compiler.value().disassemble(on_dx12.value().dxil);
        REQUIRE(text.has_value());
        CHECK(text.value().contains("@dx.op.rayQuery_Proceed")).dump("entry", entry);
        CHECK(text.value().contains("@dx.op.rayQuery_Abort")).dump("entry", entry);

        auto const on_vulkan = sgl::compile_to_text({.source = source,
                                                     .entry_point = entry,
                                                     .target = sgl::emit::target::hlsl_vulkan,
                                                     .native = sgl::emit::native_route::require});
        REQUIRE(on_vulkan.has_value()).dump("entry", entry);
        spirv_modules.add(on_vulkan.value().spirv, "tests/corpus/types/queries.sgl", entry);
    }
    if (has_spirv_val)
        spirv_modules.validate();
}

// A required-native package hard-errors on a gap, and a feature the test machine lacks hides one.
// So every fixture is compiled as a device with these would, and what falls to a gap is named here to be seen.
// A task per entry point: every one compiles twice, and the viewer's hybrid pass alone holds seconds of them.
ASYNC_TEST("slib - a device's micromaps and reordering leave only the named gaps on either route")
{
    if constexpr (!k_has_validator)
        SKIP("DXC's validator is dxil.dll, which only Windows has");
    if (ssc::dxc::compiler::create().has_error())
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
    auto sources = cc::vector<cc::string>();
    for (auto const& file : files)
        sources.push_back(read_file(file));

    auto entries = cc::vector<cc::vector<cc::string>>::create_defaulted(files.size());
    co_await cc::async_parallel_for(files.size(),
                                    [&](isize f)
                                    {
                                        auto const all
                                            = sgl::compile_all_to_text({.source = sources[f],
                                                                        .source_name = files[f],
                                                                        .library = library.files,
                                                                        .targets = {sgl::emit::target::hlsl_dx12}});
                                        if (all.has_value())
                                            for (auto const& entry : all.value())
                                                entries[f].push_back(entry.entry_point);
                                    },
                                    {.grain = 1});

    struct job
    {
        isize file = 0;
        cc::string entry_point;
        int lowered = 0;
        int refused = 0;
        cc::vector<u32> spirv;
    };
    auto jobs = cc::vector<job>();
    for (auto f = isize(0); f < files.size(); ++f)
        for (auto const& entry_point : entries[f])
            jobs.push_back({.file = f, .entry_point = entry_point});

    co_await cc::async_parallel_for(
        jobs.size(),
        [&](isize j)
        {
            auto& job = jobs[j];
            auto const& file = files[job.file];
            for (auto const target : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
            {
                auto const e = sgl::compile_to_text({.source = sources[job.file],
                                                     .source_name = file,
                                                     .library = library.files,
                                                     .entry_point = job.entry_point,
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
                        .dump("entry", job.entry_point)
                        .dump("gap", e.value().native_gap);
                    CC_LOG_INFO("device gap {}: {}", target == sgl::emit::target::hlsl_dx12 ? "dxil" : "spirv",
                                e.value().native_gap);
                    continue;
                }
                ++job.lowered;
                if (target != sgl::emit::target::hlsl_dx12)
                {
                    if (has_spirv_val)
                        job.spirv = cc::vector<u32>::create_copy_of(e.value().spirv);
                    continue;
                }
                // a compiler is not thread-safe, so each task takes its own
                auto compiler = ssc::dxc::compiler::create();
                REQUIRE(compiler.has_value());
                auto const refused = compiler.value().validate(e.value().dxil);
                job.refused += refused.has_value() ? 1 : 0;
                CHECK(!refused.has_value())
                    .dump("file", file)
                    .dump("entry", job.entry_point)
                    .dump("validator", refused.value_or(cc::string()));
            }
        },
        {.grain = 1});

    auto spirv_modules = spirv_batch();
    auto refused_count = 0;
    auto lowered = 0;
    for (auto const& job : jobs)
    {
        lowered += job.lowered;
        refused_count += job.refused;
        if (!job.spirv.empty())
            spirv_modules.add(job.spirv, files[job.file], job.entry_point);
    }
    spirv_modules.validate();
    CC_LOG_INFO("device features: {} lowered, {} refused", lowered, refused_count);
    CHECK(lowered >= 690);
}
