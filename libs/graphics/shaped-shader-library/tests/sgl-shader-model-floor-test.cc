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

    auto files = sources_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    files.push_back_range(sources_under(nx::source_path("libs/graphics/shaped-graphics-language/tests/corpus")));
    REQUIRE(!files.empty());

    auto const stride = nx::is_thorough() ? 1 : 4;
    auto stages = cc::vector<sgl::check::stage>();
    auto compiled = 0;
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
        // what names a module of another directory is the module tests', and this reads a file alone
        auto const all = sgl::compile_all_to_text(
            {.source = source,
             .targets = {sgl::emit::target::hlsl_dx12},
             .core = {.draw_start = sgl::check::fill::root_constant, .wave_size = sgl::check::fill::none}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            // an entry that takes the draw's start is refused under the root-constant fill, as shaped-graphics-language's tests hold
            if (!entry.text.has_value())
                continue;
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
            // a text this test cannot hand DXC alone at the profile slib compiles is no question of the floor
            if (auto const reference = at(ssc::dxc::shader_model::sm_6_8); reference.has_error())
            {
                CC_LOG_INFO("{}: {}: not compiled alone at 6.8 either: {}", file, entry.entry_point,
                            reference.error().to_string());
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
    for (auto const s :
         {sgl::check::stage::vertex, sgl::check::stage::pixel, sgl::check::stage::compute, sgl::check::stage::geometry,
          sgl::check::stage::tessellation_control, sgl::check::stage::tessellation_evaluation,
          sgl::check::stage::raygen, sgl::check::stage::miss, sgl::check::stage::closest_hit})
        CHECK(has(stages, s)).dump("stage", sgl::check::stage_name(s));
}
