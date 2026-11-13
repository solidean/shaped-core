#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <nexus/tests/thorough.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// EMIT-262: with no feature granted and the 6.6 core fills, SGL's dx12 text compiles with DXC at the Shader Model 6.6
// profiles, every stage, given the arguments the compile reports beside it.
// SGL's own DXIL states its model in its container, which shaped-graphics-language's sweep reads back; the text needs DXC.

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

[[nodiscard]] cc::vector<cc::string> sources_under(cc::string_view root)
{
    namespace fs = std::filesystem;
    auto found = cc::vector<cc::string>();
    auto ec = std::error_code();
    for (auto const& e : fs::recursive_directory_iterator(fs::path(root.data(), root.data() + root.size()), ec))
        if (e.is_regular_file() && e.path().extension() == ".sgl")
            found.push_back(cc::string(e.path().generic_string().c_str()));
    cc::sort(found);
    return found;
}

[[nodiscard]] sg::shader_stage stage_of(sgl::check::stage s)
{
    switch (s)
    {
    case sgl::check::stage::vertex:
        return sg::shader_stage::vertex;
    case sgl::check::stage::pixel:
        return sg::shader_stage::fragment;
    case sgl::check::stage::geometry:
        return sg::shader_stage::geometry;
    case sgl::check::stage::tessellation_control:
        return sg::shader_stage::tessellation_control;
    case sgl::check::stage::tessellation_evaluation:
        return sg::shader_stage::tessellation_evaluation;
    case sgl::check::stage::task:
        return sg::shader_stage::task;
    case sgl::check::stage::mesh:
        return sg::shader_stage::mesh;
    case sgl::check::stage::raygen:
        return sg::shader_stage::raygen;
    case sgl::check::stage::miss:
        return sg::shader_stage::miss;
    case sgl::check::stage::closest_hit:
        return sg::shader_stage::closest_hit;
    case sgl::check::stage::any_hit:
        return sg::shader_stage::any_hit;
    case sgl::check::stage::intersection:
        return sg::shader_stage::intersection;
    case sgl::check::stage::callable:
        return sg::shader_stage::callable;
    default:
        return sg::shader_stage::compute;
    }
}
/// The module directories of the repo, which a swept file may `use`: read once, viewed by `files`.
struct module_library
{
    cc::vector<cc::string> names;
    cc::vector<cc::string> sources;
    cc::vector<sgl::library_file> files;

    void read()
    {
        for (auto const& dir : {nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl/modules"),
                                nx::source_path("libs/graphics/shaped-rendering/shaders/modules"),
                                nx::source_path("libs/graphics/shaped-graphics-language/tests/corpus/modules"),
                                nx::source_path("libs/graphics/shaped-viewer/shaders/sgl")})
            for (auto const& file : sources_under(dir))
            {
                names.push_back(file);
                sources.push_back(read_file(file));
            }
        for (auto i = isize(0); i < names.size(); ++i)
            files.push_back({.name = names[i], .source = sources[i]});
    }
};

/// Every file the sweeps read: every directory of the repo's SGL shaders, the examples' and the corpus.
[[nodiscard]] cc::vector<cc::string> swept_files()
{
    auto files = sources_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl_mixed")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-graphics/backends/metal/tests/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-xr/shaders")));
    files.push_back_range(sources_under(nx::source_path("examples/graphics/ray-throughput/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-rendering/tests/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-viewer/shaders")));
    files.push_back_range(sources_under(nx::source_path("examples/graphics/slug-traced/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-graphics-language/tests/corpus")));
    return files;
}

/// The entries whose text DXC refuses at every profile, each by its file and its name, with the error DXC gives and
/// the issue that tracks it; any other failure, of SGL or of DXC, fails the sweep.
struct listed
{
    /// The end of the file's path, from a directory the sweep reads.
    cc::string_view file;
    cc::string_view entry;
    /// What DXC's error says of it, at 6.6 and at 6.8 alike.
    cc::string_view refusal;
    cc::string_view issue;
};
constexpr auto k_fused = cc::string_view("bug-sgl-dx12-fused-intersection-takes-parameters");
constexpr auto k_empty = cc::string_view("bug-sgl-dx12-empty-closest-hit-malformed");
constexpr auto k_no_parameters = cc::string_view("expected no parameters");
constexpr auto k_no_declarator = cc::string_view("expected parameter declarator");
constexpr listed k_not_compiled[] = {
    {"shaped-graphics/tests/shaders/sgl/raytracing_end_search.sgl", "sgl_layered_slabs_through", k_no_parameters, k_fused},
    {"shaped-graphics/tests/shaders/sgl/raytracing_pipeline.sgl", "sgl_spheres_surface", k_no_parameters, k_fused},
    {"shaped-graphics/tests/shaders/sgl/raytracing_pipeline.sgl", "sgl_spheres_occlusion", k_no_parameters, k_fused},
    {"shaped-graphics/tests/shaders/sgl_mixed/ray_stages.sgl", "sgl_balls_primary", k_no_parameters, k_fused},
    {"shaped-graphics/tests/shaders/sgl_mixed/ray_stages.sgl", "sgl_balls_shadow", k_no_parameters, k_fused},
    {"shaped-graphics/backends/metal/tests/shaders/tracing.sgl", "sgl_boxed_marker", k_no_parameters, k_fused},
    {"shaped-graphics/tests/shaders/sgl/raytracing_end_search.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/raytracing_transforms.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-rendering/tests/shaders/slug_traced_pipeline.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/raytracing_callables.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/raytracing_open.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/raytracing_pipeline.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/raytracing_scenes.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl/sticky_arrays_traced.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/tests/shaders/sgl_mixed/ray_stages.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-graphics/backends/metal/tests/shaders/tracing.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
    {"shaped-viewer/shaders/tracer_pipeline.sgl", "sgl_empty_closest_hit", k_no_declarator, k_empty},
};

[[nodiscard]] listed const* listed_as(cc::string_view file, cc::string_view entry)
{
    for (auto const& l : k_not_compiled)
        if (file.ends_with(l.file) && entry == l.entry)
            return &l;
    return nullptr;
}

/// A SPIR-V module's instructions as words, without the ones that only name or place things: OpSource and its
/// extensions, OpName, OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed and OpDecorateString.
[[nodiscard]] cc::vector<u32> meaning_of(cc::span<byte const> bytes)
{
    auto const words = cc::span<u32 const>(reinterpret_cast<u32 const*>(bytes.data()), bytes.size() / 4);
    auto result = cc::vector<u32>();
    // the header but its generator word, which names the tool
    for (auto i = isize(0); i < 5 && i < words.size(); ++i)
        result.push_back(i == 2 ? 0u : words[i]);
    for (auto at = isize(5); at < words.size();)
    {
        auto const count = isize(words[at] >> 16);
        auto const opcode = words[at] & 0xffffu;
        if (count == 0)
            break;
        auto const is_naming = opcode == 3 || opcode == 4 || opcode == 5 || opcode == 6 || opcode == 7 || opcode == 8
                            || opcode == 317 || opcode == 330 || opcode == 5632;
        if (!is_naming)
            for (auto k = at; k < at + count && k < words.size(); ++k)
                result.push_back(words[k]);
        at += count;
    }
    return result;
}

/// The stages the repo's shaders hold none of, which the sweep below would otherwise never meet.
constexpr auto k_primitive_stages = cc::string_view(
    "require geometry_shader\n"
    "require tessellation_shader\n"
    "binding params:\n    level: float\n"
    "struct control_point:\n    position: float3\n"
    "struct varyings:\n    @position position: hpos4\n    color: float3\n"
    "struct tri_factors:\n"
    "    @edge_factors edges: float[3]\n"
    "    @inside_factors inside: float\n"
    "    bulge: float\n"
    "@tessellation_control(partitioning = .fractional_odd, winding = .counter_clockwise)\n"
    "fun tc(patch: control_point[3]){params} -> tri_factors:\n"
    "    return { edges = [params.level, params.level, params.level], inside = params.level, bulge = 0.0 }\n"
    "@tessellation_evaluation\n"
    "fun te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3) -> varyings:\n"
    "    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z\n"
    "    return { position = hpos4(p.x, p.y, p.z + f.bulge, 1.0), color = uvw }\n"
    "@geometry(max_vertices = 6)\n"
    "fun gs(tri: varyings[3], @primitive_id prim: int, stream: mut triangle_stream[varyings]):\n"
    "    for i in 0 ..< 3:\n"
    "        stream.emit(tri[i])\n"
    "    stream.end_strip()\n");
} // namespace

TEST("slib - with no feature granted and the 6.6 fills, SGL's dx12 text compiles at DXC's 6.6 profiles, every stage")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");

    auto const files = swept_files();
    REQUIRE(!files.empty());
    auto library = module_library();
    library.read();

    auto const stride = nx::is_thorough() ? 1 : 4;
    auto stages = cc::vector<sgl::check::stage>();
    auto compiled = 0;
    auto listed_seen = isize(0);
    auto const has = [](cc::span<sgl::check::stage const> stages, sgl::check::stage s)
    {
        for (auto const t : stages)
            if (t == s)
                return true;
        return false;
    };
    auto sources = cc::vector<cc::string>();
    auto names = cc::vector<cc::string>();
    for (auto i = isize(0); i < files.size(); i += stride)
    {
        names.push_back(files[i]);
        sources.push_back(read_file(files[i]));
    }
    names.push_back(cc::string("geometry and tessellation"));
    sources.push_back(cc::string(k_primitive_stages));
    for (auto i = isize(0); i < sources.size(); ++i)
    {
        auto const& file = names[i];
        auto const& source = sources[i];
        auto const all = sgl::compile_all_to_text(
            {.source = source,
             .source_name = file,
             .library = library.files,
             .targets = {sgl::emit::target::hlsl_dx12},
             .core = {.draw_start = sgl::check::fill::root_constant, .wave_size = sgl::check::fill::none}});
        if (all.has_error())
        {
            CHECK(false).dump("file", file).dump("error", all.error());
            continue;
        }
        for (auto const& entry : all.value())
        {
            // an entry that takes the draw's start is refused under the root-constant fill, as shaped-graphics-language's
            // tests hold; any other error is the sweep's to report
            if (!entry.text.has_value())
            {
                CHECK(entry.text.error().contains("asks the root-constant fill of draw_start, which is not written "
                                                  "yet"))
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("error", entry.text.error());
                continue;
            }
            auto const& e = entry.text.value();
            REQUIRE(e.dxil_shader_model == sgl::check::dxil_shader_model{});
            // what slib hands DXC beside SGL's text (EMIT-4, EMIT-185)
            auto options = ssc::dxc::compile_options();
            options.extra_args.push_back(cc::string("-enable-16bit-types"));
            // and what the compile reports the text needs at its model (EMIT-262)
            for (auto const& arg : e.dxc_args)
                options.extra_args.push_back(arg);
            if (e.float_guarantee != sgl::check::float_level::fast)
                options.extra_args.push_back(cc::string("-Gis"));
            if (e.float_guarantee == sgl::check::float_level::ieee)
            {
                options.extra_args.push_back(cc::string("-denorm"));
                options.extra_args.push_back(cc::string("preserve"));
            }
            auto const at = [&](ssc::dxc::shader_model model)
            {
                return compiler.value().compile(
                    {.source = e.text, .entry_point = e.entry_point, .stage = stage_of(e.stage), .model = model},
                    options);
            };
            // a text DXC refuses at every profile is listed with its refusal and its issue, and no question of the
            // floor; it is refused as listed at 6.6 and at 6.8, or the list is wrong
            if (auto const* const l = listed_as(file, entry.entry_point); l != nullptr)
            {
                for (auto const model : {ssc::dxc::shader_model::sm_6_6, ssc::dxc::shader_model::sm_6_8})
                {
                    auto const refused = at(model);
                    CHECK((refused.has_error() && refused.error().to_string().contains(l->refusal)))
                        .dump("file", file)
                        .dump("entry", entry.entry_point)
                        .dump("issue", l->issue)
                        .dump("dxc", refused.has_error() ? refused.error().to_string() : cc::string("compiled"));
                }
                ++listed_seen;
                continue;
            }
            auto const built = at(ssc::dxc::shader_model::sm_6_6);
            // a ray-tracing stage that states its payload's access qualifiers says so, which 6.6 takes only with the flag
            if (e.stage >= sgl::check::stage::raygen && e.text.contains("[raypayload]"))
                CHECK((e.dxc_args.size() == 1 && e.dxc_args[0] == "-enable-payload-qualifiers"))
                    .dump("entry", entry.entry_point);
            CHECK(built.has_value())
                .dump("file", file)
                .dump("entry", entry.entry_point)
                .dump("dxc", built.has_error() ? built.error().to_string() : cc::string());
            ++compiled;
            if (!has(stages, e.stage))
                stages.push_back(e.stage);
        }
    }
    auto stage_names = cc::string();
    for (auto const s : stages)
        stage_names.appendf("{}{}", stage_names.empty() ? "" : ", ", sgl::check::stage_name(s));
    CC_LOG_INFO("{} dx12 texts compiled at Shader Model 6.6, of {}", compiled, stage_names);
    // the whole sweep meets every entry the list names, so none stands there that no longer fails
    if (nx::is_thorough())
        CHECK(listed_seen == isize(sizeof(k_not_compiled) / sizeof(k_not_compiled[0])));
    for (auto const s :
         {sgl::check::stage::vertex, sgl::check::stage::pixel, sgl::check::stage::compute, sgl::check::stage::geometry,
          sgl::check::stage::tessellation_control, sgl::check::stage::tessellation_evaluation,
          sgl::check::stage::raygen, sgl::check::stage::miss, sgl::check::stage::closest_hit})
        CHECK(has(stages, s)).dump("stage", sgl::check::stage_name(s));
}

// EMIT-128: a vulkan vertex entry reads where the draw started as `BaseVertex` and `BaseInstance`, which every shader
// model has, where the text read Shader Model 6.8's system values before; DXC makes the same SPIR-V of both, so what sg
// runs is what it ran.
TEST("slib - a vulkan entry's draw start as builtins is the SPIR-V DXC made of the system values before (EMIT-128)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const files = swept_files();
    auto library = module_library();
    library.read();
    auto options = ssc::dxc::compile_options{.target = ssc::dxc::compile_target::spirv};
    options.extra_args.push_back(cc::string("-enable-16bit-types"));
    auto compared = 0;
    for (auto const& file : files)
    {
        auto const source = read_file(file);
        auto const all = sgl::compile_all_to_text(
            {.source = source, .source_name = file, .library = library.files, .targets = {sgl::emit::target::hlsl_vulkan}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            if (!entry.text.has_value() || !entry.text.value().text.contains("SGL_BASE_"))
                continue;
            auto const& e = entry.text.value();
            // the text as it was: the same parameters, spelled as the system values
            auto before = cc::string(e.text);
            before.replace_all("[[vk::builtin(\"BaseVertex\")]] uint", "uint");
            before.replace_all("[[vk::builtin(\"BaseInstance\")]] uint", "uint");
            before.replace_all(": SGL_BASE_VERTEX", ": SV_StartVertexLocation");
            before.replace_all(": SGL_BASE_INSTANCE", ": SV_StartInstanceLocation");
            REQUIRE(!before.contains("vk::builtin(\"Base"));
            auto const desc = ssc::dxc::shader_description{.source = e.text,
                                                           .entry_point = e.entry_point,
                                                           .stage = stage_of(e.stage),
                                                           .model = ssc::dxc::shader_model::sm_6_8};
            auto old_desc = desc;
            old_desc.source = before;
            auto const now = compiler.value().compile(desc, options);
            auto const then = compiler.value().compile(old_desc, options);
            REQUIRE(now.has_value())
                .dump("file", file)
                .dump("entry", entry.entry_point)
                .dump("dxc", now.has_error() ? now.error().to_string() : cc::string());
            REQUIRE(then.has_value())
                .dump("file", file)
                .dump("entry", entry.entry_point)
                .dump("dxc", then.has_error() ? then.error().to_string() : cc::string());
            auto const a = meaning_of(now.value().bytecode.span());
            auto const b = meaning_of(then.value().bytecode.span());
            CHECK(a.size() == b.size()).dump("file", file).dump("entry", entry.entry_point);
            auto same = a.size() == b.size();
            for (auto i = isize(0); same && i < a.size(); ++i)
                same = a[i] == b[i];
            CHECK(same).dump("file", file).dump("entry", entry.entry_point);
            ++compared;
        }
    }
    CC_LOG_INFO("{} vulkan entries that read the draw's start make the SPIR-V they made before", compared);
    // the repo's vertex entries that read the draw's start, of which the review counted 34
    CHECK(compared >= 34);
}
