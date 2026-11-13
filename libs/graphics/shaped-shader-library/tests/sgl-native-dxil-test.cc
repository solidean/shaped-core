#include <clean-core/algorithm/sort.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-shader-compiler-dxc/compiler.hh>

#include <filesystem>

using namespace cc::primitive_defines;

// What SGL writes as DXIL has to pass the validator a signed shader passes, though nothing ships through it.

namespace
{
[[nodiscard]] cc::string read_file(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

struct kernel
{
    cc::string_view path;
    cc::string_view entry;
};

constexpr kernel k_kernels[] = {
    {"libs/graphics/shaped-graphics/tests/shaders/sgl/dispatch.sgl", "stamp"},
    {"libs/graphics/shaped-rendering/shaders/atrous_denoise.sgl", "main_cs"},
    {"libs/graphics/shaped-rendering/shaders/svgf.sgl", "atrous_cs"},
    {"libs/graphics/shaped-rendering/shaders/nn_conv.sgl", "main_cs"},
};

/// A sum of three products, which SGL's reorder of `fast` additions chains, and which `@float` above `fast` keeps.
constexpr cc::string_view k_sums = "binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "@compute(64) fun fast_sum(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    work.values[id.x] += a * 2.0 + a * 3.0 + a * 5.0\n"
                                   "\n"
                                   "@float(.precise) @compute(64) fun strict_sum(@thread_id id: int3){work}:\n"
                                   "    let a = work.values[0]\n"
                                   "    work.values[id.x] += a * 2.0 + a * 3.0 + a * 5.0\n";

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(cc::string_view source, cc::string_view entry)
{
    return sgl::compile_to_text({.source = source,
                                 .entry_point = entry,
                                 .target = sgl::emit::target::hlsl_dx12,
                                 .native = sgl::emit::native_route::require});
}

/// The name of every `.sgl` file under `root`, sorted, so a failure reads the same on every run.
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
} // namespace

TEST("slib - SGL's own DXIL of real kernels passes the validator")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    for (auto const& k : k_kernels)
    {
        auto const source = read_file(nx::source_path(k.path));
        auto const e = written(source, k.entry);
        REQUIRE(e.has_value());
        REQUIRE(!e.value().dxil.empty());
        auto const refused = compiler.value().validate(e.value().dxil);
        CHECK(!refused.has_value()).dump("entry", k.entry).dump("validator", refused.value_or(cc::string()));
    }
}

TEST("slib - every entry point of the fixtures that SGL's DXIL lowers passes the validator")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto files = files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl"));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-graphics/tests/shaders/sgl_mixed")));
    files.push_back_range(files_under(nx::source_path("libs/graphics/shaped-rendering/shaders")));
    REQUIRE(!files.empty());

    auto reached = 0;
    auto lowered = 0;
    auto raster_reached = 0;
    auto raster_lowered = 0;
    auto primitive_reached = 0;
    auto primitive_lowered = 0;
    auto mesh_reached = 0;
    auto mesh_lowered = 0;
    auto ray_reached = 0;
    auto ray_lowered = 0;
    auto recording = 0;
    auto guarded = 0;
    // A profile compiles and validates an entry again, which a debug build pays seconds for, so a default run samples them.
    auto const guard_stride = nx::is_thorough() ? 1 : 8;
    auto validated = 0;
    for (auto const& file : files)
    {
        auto const source = read_file(file);
        // what names a module of another directory is the module tests', and this one reads a file alone
        auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_dx12}});
        if (all.has_error())
            continue;
        for (auto const& entry : all.value())
        {
            auto const e = sgl::compile_to_text({.source = source,
                                                 .entry_point = entry.entry_point,
                                                 .target = sgl::emit::target::hlsl_dx12,
                                                 .diagnostics = true,
                                                 .native = sgl::emit::native_route::prefer});
            if (e.has_error())
                continue;
            auto const stage = e.value().stage;
            auto const is_raster = stage == sgl::check::stage::vertex || stage == sgl::check::stage::pixel;
            auto const is_primitive = stage == sgl::check::stage::geometry
                                   || stage == sgl::check::stage::tessellation_control
                                   || stage == sgl::check::stage::tessellation_evaluation;
            auto const is_mesh = stage == sgl::check::stage::mesh || stage == sgl::check::stage::task;
            auto const is_ray = stage >= sgl::check::stage::raygen;
            if (stage != sgl::check::stage::compute && !is_raster && !is_primitive && !is_mesh && !is_ray)
                continue;
            ++(is_ray         ? ray_reached
               : is_mesh      ? mesh_reached
               : is_primitive ? primitive_reached
               : is_raster    ? raster_reached
                              : reached);
            if (e.value().dxil.empty())
            {
                CC_LOG_INFO("{}: {}: {}", file, entry.entry_point, e.value().native_gap);
                continue;
            }
            ++(is_ray         ? ray_lowered
               : is_mesh      ? mesh_lowered
               : is_primitive ? primitive_lowered
               : is_raster    ? raster_lowered
                              : lowered);
            recording += e.value().debug_sites.empty() ? 0 : 1;
            auto const refused = compiler.value().validate(e.value().dxil);
            CHECK(!refused.has_value())
                .dump("file", file)
                .dump("entry", entry.entry_point)
                .dump("validator", refused.value_or(cc::string()));

            if (validated++ % guard_stride != 0)
                continue;

            // each robustness profile through the validator alone: nothing here runs what a guard stops
            for (auto const guard : {sgl::check::undefined_guard::abort, sgl::check::undefined_guard::report,
                                     sgl::check::undefined_guard::define})
            {
                auto const profile = sgl::compile_to_text({.source = source,
                                                           .entry_point = entry.entry_point,
                                                           .target = sgl::emit::target::hlsl_dx12,
                                                           .diagnostics = true,
                                                           .guards_undefined = guard,
                                                           .native = sgl::emit::native_route::prefer});
                if (profile.has_error() || profile.value().dxil.empty())
                {
                    if (profile.has_value())
                        CC_LOG_INFO("{}: {}: under {}: {}", file, entry.entry_point, int(guard),
                                    profile.value().native_gap);
                    continue;
                }
                ++guarded;
                auto const refused_guarded = compiler.value().validate(profile.value().dxil);
                CHECK(!refused_guarded.has_value())
                    .dump("file", file)
                    .dump("entry", entry.entry_point)
                    .dump("guard", int(guard))
                    .dump("validator", refused_guarded.value_or(cc::string()));
            }
        }
    }
    CC_LOG_INFO("SGL's DXIL lowers {} of the {} compute entry points, {} of the {} vertex and pixel ones, {} of the {} "
                "geometry and tessellation ones, and {} of the {} mesh and task ones",
                lowered, reached, raster_lowered, raster_reached, primitive_lowered, primitive_reached, mesh_lowered,
                mesh_reached);
    CC_LOG_INFO("and {} of the {} ray-tracing ones", ray_lowered, ray_reached);
    CC_LOG_INFO("{} of them write debug records, and the robustness profiles lower {} entry points", recording, guarded);
    // the numbers the slice reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CHECK(lowered >= 120);
    CHECK(ray_lowered >= 50);
    CHECK(raster_lowered == raster_reached);
    CHECK(raster_lowered >= 82);
    CHECK(primitive_lowered == primitive_reached);
    CHECK(mesh_reached >= 13);
    CHECK(mesh_lowered == mesh_reached);
    CHECK(primitive_lowered >= 18);
    CHECK(recording >= 2);
    CHECK(guarded >= 840 / guard_stride);
}

TEST("slib - a 64-bit atomic is the i64 form of DXIL's own, and states what it reaches (EMIT-247)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source = "require shader_int64, shader_int64_atomics, shader_shared_int64_atomics\n"
                                       "binding results:\n"
                                       "    totals: mut buffer[atomic[ulong]]\n"
                                       "    plain: mut buffer[ulong]\n"
                                       "\n"
                                       "@workgroup binding local:\n"
                                       "    lowest: atomic[long]\n"
                                       "\n"
                                       "@compute(64) fun in_buffer(@thread_id id: int3){results}:\n"
                                       "    results.totals[0].add(1u64 << 33)\n"
                                       "    results.plain[id.x] = results.totals[0].min(5u64)\n"
                                       "\n"
                                       "@compute(64) fun in_workgroup(@local_thread_index t: int){results, local}:\n"
                                       "    local.lowest.min(-(1i64 << 40) - (t as long))\n"
                                       "    workgroup_barrier()\n"
                                       "    results.plain[t] = local.lowest.load() as ulong\n";
    auto const buffer = written(source, "in_buffer");
    REQUIRE(buffer.has_value());
    CHECK(!compiler.value().validate(buffer.value().dxil).has_value());
    auto const buffer_text = compiler.value().disassemble(buffer.value().dxil);
    REQUIRE(buffer_text.has_value());
    CHECK(buffer_text.value().contains("@dx.op.atomicBinOp.i64(i32 78"));
    // the resource a 64-bit atomic reaches carries Atomic64Use, tag 3, beside its stride, and PSV0 says the same
    CHECK(buffer_text.value().contains("!{i32 1, i32 8, i32 3, i32 1}"));
    CHECK(!buffer_text.value().contains("64-bit Atomics on Group Shared"));

    auto const shared = written(source, "in_workgroup");
    REQUIRE(shared.has_value());
    CHECK(!compiler.value().validate(shared.value().dxil).has_value());
    auto const shared_text = compiler.value().disassemble(shared.value().dxil);
    REQUIRE(shared_text.has_value());
    CHECK(shared_text.value().contains("atomicrmw min i64 addrspace(3)*"));
    CHECK(shared_text.value().contains("64-bit Atomics on Group Shared"));
}

TEST("slib - a debug record of every value family passes the validator, in each stage that writes one")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source
        = "require shader_f16\n"
          "enum tone:\n"
          "    low\n"
          "    high\n"
          "\n"
          "struct link:\n"
          "    @position position: hpos4\n"
          "\n"
          "@pixel struct written:\n"
          "    color: float4\n"
          "\n"
          "@compute(4) fun records_cs(@thread_id id: int3):\n"
          "    print \"$(id.x) $(id.x as uint) $(id.x as float) $(half(0.5)) $(id.x > 1) $id $(tone.high)\"\n"
          "\n"
          "@vertex fun records_vs(@vertex_index v: int) -> link:\n"
          "    print \"vertex $v\"\n"
          "    return { position = hpos4(0.0, 0.0, 0.0, 1.0) }\n"
          "\n"
          "@pixel fun records_ps(l: link) -> written:\n"
          "    print \"pixel $(l.position.x)\"\n"
          "    return { color = float4(1.0) }\n";
    for (auto const entry : {"records_cs", "records_vs", "records_ps"})
    {
        auto const e = sgl::compile_to_text({.source = source,
                                             .entry_point = entry,
                                             .target = sgl::emit::target::hlsl_dx12,
                                             .diagnostics = true,
                                             .native = sgl::emit::native_route::require});
        REQUIRE(e.has_value());
        REQUIRE(e.value().debug_sites.size() == 1);
        auto const refused = compiler.value().validate(e.value().dxil);
        CHECK(!refused.has_value()).dump("entry", entry).dump("validator", refused.value_or(cc::string()));
    }

    // a UAV outside a pixel or a compute stage is a feature the container states, which DXC's disassembly reads back
    auto const vertex = sgl::compile_to_text({.source = source,
                                              .entry_point = "records_vs",
                                              .target = sgl::emit::target::hlsl_dx12,
                                              .diagnostics = true,
                                              .native = sgl::emit::native_route::require});
    REQUIRE(vertex.has_value());
    auto const text = compiler.value().disassemble(vertex.value().dxil);
    REQUIRE(text.has_value());
    CHECK(text.value().contains("UAVs at every shader stage"));
}

TEST("slib - a pixel stage's surface is DXIL's own operations, and a binding array's element its own handle")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source
        = "require binding_arrays\n"
          "struct link:\n"
          "    @position position: hpos4\n"
          "    near: float2\n"
          "\n"
          "@pixel struct written:\n"
          "    color: float4\n"
          "    @depth depth: float\n"
          "\n"
          "binding shelf:\n"
          "    images: texture_2d[float4][2]\n"
          "\n"
          "@pixel fun surface_ps(l: link, @is_front_facing front: bool){shelf} -> written:\n"
          "    if not front => discard\n"
          "    let at = (l.position.x as int) % 2\n"
          "    return { color = shelf.images[nonuniform at].load(int2(0, 0)) * ddx(l.near.x), depth = 0.5 }\n";
    auto const e = written(source, "surface_ps");
    REQUIRE(e.has_value());
    CHECK(!compiler.value().validate(e.value().dxil).has_value());
    auto const text = compiler.value().disassemble(e.value().dxil);
    REQUIRE(text.has_value());
    CHECK(text.value().contains("@dx.op.discard(i32 82, i1 true)"));
    CHECK(text.value().contains("@dx.op.unary.f32(i32 83,"));
    // the second argument after the binding is the index, and the last says it differs between invocations
    CHECK(text.value().contains(", i1 true)  ; CreateHandleFromBinding"));
    CHECK(text.value().contains("SV_Depth"));
}

TEST("slib - a control stage is two functions on DXIL, and an evaluation stage reads what it wrote")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source
        = "require geometry_shader\n"
          "require tessellation_shader\n"
          "struct point:\n"
          "    at: float2\n"
          "\n"
          "struct factors:\n"
          "    tint: float\n"
          "    @edge_factors edges: float[3]\n"
          "    @inside_factors inside: float\n"
          "\n"
          "struct link:\n"
          "    @position position: hpos4\n"
          "\n"
          "@tessellation_control(partitioning = .fractional_odd, winding = .clockwise)\n"
          "fun control(patch: point[3]) -> factors:\n"
          "    return { tint = 1.0, edges = [patch[0].at.x, 2.0, 3.0], inside = 4.0 }\n"
          "\n"
          "@tessellation_evaluation\n"
          "fun evaluate(patch: point[3], f: factors, @domain_location uvw: float3, @primitive_id id: int) -> link:\n"
          "    return { position = hpos4(uvw.x * patch[0].at.x, f.tint, f.edges[1], id as float) }\n"
          "\n"
          "@geometry(max_vertices = 3)\n"
          "fun copied(tri: link[3], stream: mut triangle_stream[link]):\n"
          "    for i in 0 ..< 3:\n"
          "        stream.emit(tri[i])\n"
          "    stream.end_strip()\n";
    auto const control = written(source, "control");
    REQUIRE(control.has_value());
    CHECK(!compiler.value().validate(control.value().dxil).has_value());
    auto const hull = compiler.value().disassemble(control.value().dxil);
    REQUIRE(hull.has_value());
    CHECK(hull.value().contains("define void @control_patch()"));
    CHECK(hull.value().contains("define void @control()"));
    CHECK(hull.value().contains("@dx.op.outputControlPointID.i32(i32 107)"));
    // the array is one element of three rows in `dx.sig`, and three lines of `PSG1` a disassembly prints
    CHECK(hull.value().contains("@dx.op.storePatchConstant.f32(i32 106, i32 1, i32 2, i8 0,"));
    // EMIT-134: the domain's own orientation mirrors the `.clockwise` the source names
    CHECK(hull.value().contains("OutputPrimitive=triangle_ccw"));

    auto const evaluate = written(source, "evaluate");
    REQUIRE(evaluate.has_value());
    CHECK(!compiler.value().validate(evaluate.value().dxil).has_value());
    auto const domain = compiler.value().disassemble(evaluate.value().dxil);
    REQUIRE(domain.has_value());
    CHECK(domain.value().contains("@dx.op.loadPatchConstant.f32(i32 104, i32 1, i32 1, i8 0)"));
    CHECK(domain.value().contains("@dx.op.domainLocation.f32(i32 105, i8 0)"));
    CHECK(domain.value().contains("@dx.op.primitiveID.i32(i32 108)"));

    auto const geometry = written(source, "copied");
    REQUIRE(geometry.has_value());
    CHECK(!compiler.value().validate(geometry.value().dxil).has_value());
    auto const stream = compiler.value().disassemble(geometry.value().dxil);
    REQUIRE(stream.has_value());
    CHECK(stream.value().contains("@dx.op.emitStream(i32 97, i8 0)"));
    CHECK(stream.value().contains("@dx.op.cutStream(i32 98, i8 0)"));
}

TEST("slib - DXIL's float flags stand on each operation, and only `fast` lets a sum reorder")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const fast = written(k_sums, "fast_sum");
    auto const strict = written(k_sums, "strict_sum");
    REQUIRE(fast.has_value());
    REQUIRE(strict.has_value());
    auto const fast_text = compiler.value().disassemble(fast.value().dxil);
    auto const strict_text = compiler.value().disassemble(strict.value().dxil);
    REQUIRE(fast_text.has_value());
    REQUIRE(strict_text.has_value());
    CHECK(fast_text.value().contains("fadd fast float"));
    CHECK(!strict_text.value().contains("fast float"));
    CHECK(strict_text.value().contains("fadd float"));
}

// AMD's DXIL compiler crashes on a module without it, though LLVM leaves it optional.
// docs/bugs-external/amd-d3d12-dxil-without-llvm-ident-crashes has the reproduction.
TEST("slib - every module SGL's DXIL writes names its producer in llvm.ident, a library's too")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const kernel = written(k_sums, "fast_sum");
    constexpr cc::string_view miss_source = "require raytracing_pipeline\n"
                                            "struct probe:\n"
                                            "    is_missed: bool\n"
                                            "@miss fun probe_miss(p: mut probe):\n"
                                            "    p.is_missed = true\n";
    auto const library = written(miss_source, "probe_miss");
    REQUIRE(kernel.has_value());
    REQUIRE(library.has_value());
    for (auto const* e : {&kernel.value(), &library.value()})
    {
        auto const text = compiler.value().disassemble(e->dxil);
        REQUIRE(text.has_value());
        CHECK(text.value().contains("!llvm.ident = !{"));
        CHECK(text.value().contains("!\"sgl\""));
    }
}

// On NVIDIA, two pipelines of one device that disagree on a payload's access lose its writes, and DXC states it.
// docs/bugs-external/nvidia-reorder-payload-qualifiers-across-pipelines has the reproduction.
TEST("slib - SGL's DXIL states each payload field's access, as DXC does for SGL's text")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    // `value` is the closest hit's alone, and the miss writes `is_hit` too
    constexpr cc::string_view source
        = "require raytracing_pipeline\n"
          "binding scene:\n"
          "    world: acceleration_structure[.triangles]\n"
          "    seen: mut buffer[int]\n"
          "struct probe:\n"
          "    value: int\n"
          "    is_hit: int\n"
          "rays probe_rays:\n"
          "    surface: probe\n"
          "@raygen fun shoot(@launch_id id: int3){scene}:\n"
          "    let mut p = probe(0, 0)\n"
          "    let r = ray(origin = pos3(0.0, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0))\n"
          "    trace(scene.world, r, probe_rays.surface, mut p)\n"
          "    scene.seen[id.x] = p.value + p.is_hit\n"
          "@miss fun probe_miss(p: mut probe):\n"
          "    p.is_hit = 0\n"
          "@closest_hit fun probe_hit(h: triangle_hit, p: mut probe):\n"
          "    p.value = 10\n"
          "    p.is_hit = 1\n"
          "hit_group hits for probe_rays:\n"
          "    surface = (closest_hit = probe_hit)\n"
          "@raytracing pipeline probe_path:\n"
          "    rays = probe_rays\n"
          "    raygen = shoot\n"
          "    miss.surface = probe_miss\n"
          "    hit_groups = (hits)\n";
    struct stage_case
    {
        cc::string_view entry;
        sg::shader_stage stage;
    };
    for (auto const& c : {stage_case{"shoot", sg::shader_stage::raygen}, stage_case{"probe_miss", sg::shader_stage::miss},
                          stage_case{"probe_hit", sg::shader_stage::closest_hit}})
    {
        auto const native = written(source, c.entry);
        REQUIRE(native.has_value());
        auto const text
            = sgl::compile_to_text({.source = source, .entry_point = c.entry, .target = sgl::emit::target::hlsl_dx12});
        REQUIRE(text.has_value());
        auto const built
            = compiler.value().compile({.source = text.value().text, .entry_point = c.entry, .stage = c.stage},
                                       {.target = ssc::dxc::compile_target::dxil, .warnings_as_errors = false});
        REQUIRE(built.has_value());
        auto const ours = compiler.value().disassemble(native.value().dxil);
        auto const theirs = compiler.value().disassemble(built.value().bytecode);
        REQUIRE(ours.has_value());
        REQUIRE(theirs.has_value());
        // read and written by the caller and the closest hit; and by the miss too
        for (auto const* expected :
             {"!dx.dxrPayloadAnnotations = !{", "%struct.probe undef", "!{i32 0, i32 51}", "!{i32 0, i32 819}"})
        {
            CHECK(theirs.value().contains(expected)).context(cc::format("{} through DXC: {}", c.entry, expected));
            CHECK(ours.value().contains(expected)).context(cc::format("{} in SGL's DXIL: {}", c.entry, expected));
        }
        CHECK(!compiler.value().validate(native.value().dxil).has_value());
    }
}

TEST("slib - a container SGL writes carries the digest the retail runtime takes unsigned, and its stage's parts")
{
    auto const e = written(k_sums, "fast_sum");
    REQUIRE(e.has_value());
    auto const bytes = cc::span<byte const>(e.value().dxil);
    REQUIRE(bytes.size() > 32);
    CHECK(u8(bytes[0]) == 'D');
    CHECK(u8(bytes[3]) == 'C');
    for (auto i = isize(4); i < 20; ++i)
        CHECK(u8(bytes[i]) == 1);
}

TEST("slib - a task stage's payload DXC's own task stage cannot write passes the validator in DXC's shape")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    constexpr cc::string_view source
        = "require mesh_shader\n"
          "struct corner:\n"
          "    @position position: hpos4\n"
          "struct inner:\n"
          "    a: float3\n"
          "    b: uint\n"
          "struct handed:\n"
          "    m: mat3\n"
          "    n: inner\n"
          "    q: int2[2]\n"
          "    last: float\n"
          "@task(1) fun ts(@workgroup_id g: int3):\n"
          "    let h: handed = {\n"
          "        m = mat3(float3(1.0), float3(2.0), float3(g.x as float))\n"
          "        n = {a = float3(1.0), b = 2u}\n"
          "        q = [int2(1, 2), int2(3, 4)]\n"
          "        last = 1.0\n"
          "    }\n"
          "    dispatch_mesh(1u, 1u, 1u, h)\n"
          "@mesh(1, vertices = 3, triangles = 1)\n"
          "fun ms(@workgroup_id g: int3, h: handed, output: mut mesh_output[corner]):\n"
          "    output.set_counts(3u, 1u)\n"
          "    let y = (h.m * float3(0.0, 1.0, 0.0)).y\n"
          "    output.vertex(0u, {position = hpos4(y, h.n.a.y, h.q[g.x].y as float, h.last)})\n"
          "    output.triangle(0u, uint3(0u, 0u, 0u))\n";
    for (auto const entry : {"ts", "ms"})
    {
        auto const e = sgl::compile_to_text({.source = source,
                                             .entry_point = entry,
                                             .target = sgl::emit::target::hlsl_dx12,
                                             .native = sgl::emit::native_route::require});
        REQUIRE(e.has_value());
        auto const refused = compiler.value().validate(e.value().dxil);
        CHECK(!refused.has_value()).dump("entry", entry).dump("validator", refused.value_or(cc::string()));
        auto const text = compiler.value().disassemble(e.value().dxil);
        REQUIRE(text.has_value());
        // the shape and the 72 bytes DXC's mesh stage reads the same source with, each member on its scalars' alignment
        CHECK(text.value().contains("%struct.handed = type { [9 x float], %struct.inner, [2 x [2 x i32]], float }"))
            .dump("text", text.value());
        CHECK(text.value().contains("i32 72}")).dump("text", text.value());
    }
}

namespace
{
/// A raygen that reorders as it traces, then traces a hit object, reads all of it and regroups by it, in one block.
constexpr cc::string_view k_reordering
    = "require raytracing_pipeline\n"
      "binding frame:\n"
      "    world: acceleration_structure[.triangles]\n"
      "    seen: mut buffer[float]\n"
      "    kinds: mut buffer[int]\n"
      "struct radiance:\n"
      "    color: float3\n"
      "rays path_rays:\n"
      "    surface: radiance\n"
      "@raygen fun shoot(@launch_id id: int3){frame}:\n"
      "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
      "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
      "    trace(frame.world, r, path_rays.surface, mut p, reorder = id.x as uint, reorder_bits = 4)\n"
      "    const if has(.shader_execution_reordering):\n"
      "        let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n"
      "        let t = h.t\n"
      "        let instance_id = h.instance_id\n"
      "        let instance_index = h.instance_index\n"
      "        let primitive_index = h.primitive_index\n"
      "        let geometry_index = h.geometry_index\n"
      "        let is_hit = h.is_hit\n"
      "        let is_miss = h.is_miss\n"
      "        h.reorder(instance_index as uint, 4)\n"
      "        h.reorder()\n"
      "        maybe_reorder(id.x as uint, 2)\n"
      "        frame.seen[id.x] = t + p.color.x\n"
      "        frame.kinds[id.x] = instance_id + instance_index + primitive_index + geometry_index\n"
      "        frame.kinds[id.x + 1] = (is_hit as int) + (is_miss as int)\n"
      "@miss fun on_miss(p: mut radiance):\n"
      "    p.color = float3(1.0, 0.0, 0.0)\n"
      "@closest_hit fun on_hit(h: triangle_hit, p: mut radiance):\n"
      "    p.color = float3(0.0, 1.0, 0.0)\n"
      "hit_group hits for path_rays:\n"
      "    surface = (closest_hit = on_hit)\n"
      "@raytracing pipeline path:\n"
      "    rays = path_rays\n"
      "    raygen = shoot\n"
      "    miss.surface = on_miss\n"
      "    hit_groups = (hits)\n";

/// `k_reordering`'s raygen on dx12 under the nvapi fill of shader execution reordering, by `route`.
[[nodiscard]] cc::result<sgl::emitted_source, cc::string> through_nvapi(sgl::emit::native_route route)
{
    sgl::check::feature_fill const fills[]
        = {{.f = sgl::check::feature::shader_execution_reordering, .how = sgl::check::fill::nvapi}};
    return sgl::compile_to_text({.source = k_reordering,
                                 .entry_point = "shoot",
                                 .target = sgl::emit::target::hlsl_dx12,
                                 .device_features = sgl::check::feature::shader_execution_reordering,
                                 .fills = fills,
                                 .native = route});
}

/// `text` with every SSA name and type name of LLVM's, `%` and what follows it, cut down to its `%`.
[[nodiscard]] cc::string without_names(cc::string_view text)
{
    auto const is_name = [](char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; };
    auto result = cc::string();
    for (auto i = isize(0); i < text.size(); ++i)
    {
        result += text[i];
        if (text[i] == '%')
            while (i + 1 < text.size() && is_name(text[i + 1]))
                ++i;
    }
    return result;
}

/// The protocol of NVAPI's shader extensions in a disassembled module: each counter bump, each store into its record
/// and each trace, in order, with every name dropped, so two writers' modules compare by what they say.
/// A trace's payload drops its `nonnull`, which DXC states of the pointer and SGL's writer does not.
[[nodiscard]] cc::vector<cc::string> protocol_of(cc::string_view disassembly)
{
    auto result = cc::vector<cc::string>();
    auto at = isize(0);
    while (at < disassembly.size())
    {
        auto end = disassembly.find('\n', at);
        if (end < 0)
            end = disassembly.size();
        auto line = disassembly.subview({.offset = at, .size = end - at});
        at = end + 1;
        if (!line.contains("@dx.op.bufferUpdateCounter") && !line.contains("@dx.op.rawBufferStore")
            && !line.contains("@dx.op.traceRay"))
            continue;
        if (line.starts_with("declare"))
            continue;
        if (auto const comment = line.find(';'); comment >= 0)
            line = line.subview({.offset = 0, .size = comment});
        if (auto const call = line.find("call "); call >= 0)
            line = line.subview({.offset = call, .size = line.size() - call});
        auto plain = without_names(line);
        plain.replace_all(" nonnull", "");
        result.push_back(cc::move(plain));
    }
    return result;
}

/// `<root>/<name>`, as std::filesystem takes it.
[[nodiscard]] std::filesystem::path path_of(cc::string_view root, cc::string_view name)
{
    return std::filesystem::path(root.data(), root.data() + root.size())
         / std::filesystem::path(name.data(), name.data() + name.size());
}

/// `extern/nvapi/.install`, where `uv run extern/nvapi/fetch-nvapi.py` puts the pinned NVAPI headers; empty where absent.
[[nodiscard]] cc::string nvapi_headers()
{
    auto const dir = nx::source_path("extern/nvapi/.install");
    auto ec = std::error_code();
    return std::filesystem::is_regular_file(path_of(dir, "nvHLSLExtns.h"), ec) ? dir : cc::string();
}

/// DXC's compile of an nvapi fill's text at Shader Model 6.6, as a host makes it: NVAPI's header on its include path,
/// and the compile's own arguments unchanged; its disassembly, or DXC's error.
[[nodiscard]] cc::result<cc::string, cc::string> dxc_of(ssc::dxc::compiler& compiler,
                                                        sgl::emitted_source const& e,
                                                        cc::string_view headers)
{
    auto options = ssc::dxc::compile_options{.warnings_as_errors = false};
    for (auto const& arg : e.dxc_args)
        options.extra_args.push_back(arg);
    auto desc = ssc::dxc::shader_description{.source = e.text,
                                             .entry_point = e.entry_point,
                                             .stage = sg::shader_stage::raygen,
                                             .model = ssc::dxc::shader_model::sm_6_6};
    auto const resolve = [&](cc::string_view path) -> cc::optional<cc::string>
    {
        auto ec = std::error_code();
        auto const file = path_of(headers, path);
        if (!std::filesystem::is_regular_file(file, ec))
            return {};
        return read_file(cc::string(file.generic_string().c_str()));
    };
    auto flat = compiler.preprocess(desc, resolve, options);
    if (flat.has_error())
        return cc::error(flat.error().to_string());
    desc.source = cc::move(flat.value().source);
    auto const built = compiler.compile(desc, options);
    if (built.has_error())
        return cc::error(built.error().to_string());
    auto text = compiler.disassemble(built.value().bytecode);
    if (text.has_error())
        return cc::error(text.error().to_string());
    return cc::move(text.value());
}
} // namespace

TEST("slib - an nvapi fill's text compiles at 6.6 through DXC on NVAPI's pinned header (EMIT-263)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const headers = nvapi_headers();
    if (headers.empty())
        SKIP("the pinned NVAPI headers are not fetched: uv run extern/nvapi/fetch-nvapi.py");
    auto const e = through_nvapi(sgl::emit::native_route::text);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    REQUIRE(e.value().needs_nvapi_header);
    REQUIRE(e.value().dxil_shader_model == sgl::check::dxil_shader_model{});
    auto const theirs = dxc_of(compiler.value(), e.value(), headers);
    REQUIRE(theirs.has_value()).dump("dxc", theirs.has_error() ? theirs.error() : cc::string());
    CHECK(theirs.value().contains("@dx.op.bufferUpdateCounter"));
    CHECK(!theirs.value().contains("dx.types.HitObject"));
}

TEST("slib - SGL's DXIL of an nvapi fill passes the validator, and says to the driver what DXC's compile of its text "
     "says")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const native = through_nvapi(sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto const refused = compiler.value().validate(native.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    // the counter UAV at u0, space13, with its counter, and no hit object of Shader Model 6.9
    CHECK(ours.value().contains("!\"g_NvidiaExt\", i32 13, i32 0, i32 1, i32 12, i1 false, i1 true, i1 false"))
        .dump("ours", ours.value());
    CHECK(!ours.value().contains("dx.types.HitObject"));

    // the same protocol as DXC's compile of the text through NVAPI's header, call by call
    auto const headers = nvapi_headers();
    if (headers.empty())
        SKIP("the pinned NVAPI headers are not fetched: uv run extern/nvapi/fetch-nvapi.py");
    auto const text = through_nvapi(sgl::emit::native_route::text);
    REQUIRE(text.has_value());
    auto const theirs = dxc_of(compiler.value(), text.value(), headers);
    REQUIRE(theirs.has_value()).dump("dxc", theirs.has_error() ? theirs.error() : cc::string());
    auto const listing = [](cc::span<cc::string const> calls)
    {
        auto out = cc::string();
        for (auto const& c : calls)
            out.appendf("{}\n", c);
        return out;
    };
    auto const expected = protocol_of(theirs.value());
    auto const written = protocol_of(ours.value());
    CHECK(!expected.empty());
    CHECK(listing(written) == listing(expected)).dump("theirs", listing(expected)).dump("ours", listing(written));
}

namespace
{
/// The protocol's calls in a disassembled module as `protocol_of` finds them, but with each SSA name renamed by where
/// it first appears among them, so two writers' modules compare by which value each call takes as well as its shape.
[[nodiscard]] cc::vector<cc::string> protocol_flow_of(cc::string_view disassembly)
{
    auto const is_name = [](char c)
    { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; };
    auto seen = cc::vector<cc::string>();
    auto result = cc::vector<cc::string>();
    auto at = isize(0);
    while (at < disassembly.size())
    {
        auto end = disassembly.find('\n', at);
        if (end < 0)
            end = disassembly.size();
        auto line = disassembly.subview({.offset = at, .size = end - at});
        at = end + 1;
        if (!line.contains("@dx.op.bufferUpdateCounter") && !line.contains("@dx.op.rawBufferStore")
            && !line.contains("@dx.op.traceRay"))
            continue;
        if (line.starts_with("declare"))
            continue;
        if (auto const comment = line.find(';'); comment >= 0)
            line = line.subview({.offset = 0, .size = comment});
        auto plain = cc::string();
        for (auto i = isize(0); i < line.size(); ++i)
        {
            if (line[i] != '%')
            {
                plain += line[i];
                continue;
            }
            auto j = i + 1;
            while (j < line.size() && is_name(line[j]))
                ++j;
            auto const name = cc::string(line.subview({.offset = i + 1, .size = j - i - 1}));
            // a type's name, `%dx.types.Handle`, stays what it is, and a handle is any one of the resource's: DXC makes
            // one per access, and SGL's writer one per resource; so is a trace's payload, which DXC copies per trace
            if (name.starts_with("dx.") || name.starts_with("struct."))
                plain.appendf("%{}", name);
            else if (plain.ends_with("%dx.types.Handle "))
                plain += "%handle";
            else if (plain.ends_with("* ") || plain.ends_with("* nonnull "))
                plain += "%payload";
            else
            {
                auto index = isize(-1);
                for (auto k = isize(0); k < seen.size(); ++k)
                    if (seen[k] == name)
                        index = k;
                if (index < 0)
                {
                    index = seen.size();
                    seen.push_back(name);
                }
                plain.appendf("%v{}", index);
            }
            i = j - 1;
        }
        plain.replace_all(" nonnull", "");
        result.push_back(cc::move(plain));
    }
    return result;
}
} // namespace

TEST("slib - SGL's DXIL of an nvapi fill hands each record and each TraceRay the values DXC's compile of its text does")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    auto const headers = nvapi_headers();
    if (headers.empty())
        SKIP("the pinned NVAPI headers are not fetched: uv run extern/nvapi/fetch-nvapi.py");
    auto const native = through_nvapi(sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    auto const text = through_nvapi(sgl::emit::native_route::text);
    REQUIRE(text.has_value());
    auto const theirs = dxc_of(compiler.value(), text.value(), headers);
    REQUIRE(theirs.has_value()).dump("dxc", theirs.has_error() ? theirs.error() : cc::string());
    auto const listing = [](cc::span<cc::string const> calls)
    {
        auto out = cc::string();
        for (auto const& c : calls)
            out.appendf("{}\n", c);
        return out;
    };
    // the hit object a record names, and the handle a TraceRay takes, is the counter value the protocol handed back
    auto const expected = protocol_flow_of(theirs.value());
    auto const written = protocol_flow_of(ours.value());
    CHECK(!expected.empty());
    CHECK(listing(written) == listing(expected)).dump("theirs", listing(expected)).dump("ours", listing(written));
}

TEST("slib - SGL's DXIL of an nvapi invocation says to the driver what DXC's compile of NvInvokeHitObject says "
     "(EMIT-263)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    // a hit object invoked through the structure its trace took, once alone and once from an array by a pinned index
    auto const source
        = cc::string("require raytracing_pipeline\n"
                     "require binding_arrays\n"
                     "binding frame:\n"
                     "    world: acceleration_structure[.triangles]\n"
                     "    worlds: acceleration_structure[.triangles][4]\n"
                     "    seen: mut buffer[float]\n"
                     "struct radiance:\n"
                     "    color: float3\n"
                     "rays path_rays:\n"
                     "    surface: radiance\n"
                     "@raygen fun shoot(@launch_id id: int3){frame}:\n"
                     "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
                     "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                     "    const if has(.shader_execution_reordering):\n"
                     "        let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n"
                     "        h.reorder()\n"
                     "        h.invoke(mut p)\n"
                     "        let k = id.x % 4\n"
                     "        let g = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
                     "        g.invoke(mut p)\n"
                     "    frame.seen[id.x] = p.color.x\n"
                     "@miss fun on_miss(p: mut radiance):\n"
                     "    p.color = float3(1.0, 0.0, 0.0)\n"
                     "@closest_hit fun on_hit(h: triangle_hit, p: mut radiance):\n"
                     "    p.color = float3(0.0, 1.0, 0.0)\n"
                     "hit_group hits for path_rays:\n"
                     "    surface = (closest_hit = on_hit)\n"
                     "@raytracing pipeline path:\n"
                     "    rays = path_rays\n"
                     "    raygen = shoot\n"
                     "    miss.surface = on_miss\n"
                     "    hit_groups = (hits)\n");
    sgl::check::feature_fill const fills[]
        = {{.f = sgl::check::feature::shader_execution_reordering, .how = sgl::check::fill::nvapi}};
    auto const of = [&](sgl::emit::native_route route)
    {
        return sgl::compile_to_text({.source = source,
                                     .entry_point = "shoot",
                                     .target = sgl::emit::target::hlsl_dx12,
                                     .device_features = sgl::check::feature::shader_execution_reordering,
                                     .fills = fills,
                                     .native = route});
    };
    auto const native = of(sgl::emit::native_route::require);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto const refused = compiler.value().validate(native.value().dxil);
    CHECK(!refused.has_value()).dump("validator", refused.value_or(cc::string()));
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());

    auto const headers = nvapi_headers();
    if (headers.empty())
        SKIP("the pinned NVAPI headers are not fetched: uv run extern/nvapi/fetch-nvapi.py");
    auto const text = of(sgl::emit::native_route::text);
    REQUIRE(text.has_value()).dump("error", text.has_error() ? text.error() : cc::string());
    REQUIRE(text.value().text.contains("NvInvokeHitObject(")).dump("text", text.value().text);
    auto const theirs = dxc_of(compiler.value(), text.value(), headers);
    REQUIRE(theirs.has_value()).dump("dxc", theirs.has_error() ? theirs.error() : cc::string());
    auto const listing = [](cc::span<cc::string const> calls)
    {
        auto out = cc::string();
        for (auto const& c : calls)
            out.appendf("{}\n", c);
        return out;
    };
    // two traces that make hit objects and two that invoke them, each record and TraceRay with DXC's values
    auto const expected = protocol_flow_of(theirs.value());
    auto const written = protocol_flow_of(ours.value());
    CHECK(listing(expected).contains("@dx.op.traceRay")).dump("theirs", listing(expected));
    CHECK(listing(written) == listing(expected)).dump("theirs", listing(expected)).dump("ours", listing(written));
}

namespace
{
/// A raygen of `body`, its lines at four spaces, over a pipeline with an array of structures beside its one structure.
[[nodiscard]] cc::string pinning(cc::string_view declarations, cc::string_view body)
{
    auto source = cc::string("require raytracing_pipeline\n"
                             "require binding_arrays\n"
                             "binding frame:\n"
                             "    world: acceleration_structure[.triangles]\n"
                             "    worlds: acceleration_structure[.triangles][4]\n"
                             "    seen: mut buffer[float]\n"
                             "    picks: buffer[int]\n"
                             "struct radiance:\n"
                             "    color: float3\n"
                             "rays path_rays:\n"
                             "    surface: radiance\n");
    source += declarations;
    source += "@raygen fun shoot(@launch_id id: int3){frame}:\n"
              "    require shader_execution_reordering\n"
              "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
              "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n";
    auto is_line_start = true;
    for (auto const ch : body)
    {
        if (is_line_start)
            source += "    ";
        source += ch;
        is_line_start = ch == '\n';
    }
    source += "    frame.seen[id.x] = p.color.x\n"
              "@miss fun on_miss(p: mut radiance):\n"
              "    p.color = float3(1.0, 0.0, 0.0)\n"
              "@closest_hit fun on_hit(h: triangle_hit, p: mut radiance):\n"
              "    p.color = float3(0.0, 1.0, 0.0)\n"
              "hit_group hits for path_rays:\n"
              "    surface = (closest_hit = on_hit)\n"
              "@raytracing pipeline path:\n"
              "    rays = path_rays\n"
              "    raygen = shoot\n"
              "    miss.surface = on_miss\n"
              "    hit_groups = (hits)\n";
    return source;
}
} // namespace

TEST("slib - the structure an nvapi trace pins compiles through DXC and the validator, in loops, branches and "
     "inlinings "
     "(EMIT-263)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    struct pin_case
    {
        cc::string_view what;
        cc::string_view declarations;
        cc::string_view body;
    };
    pin_case const cases[] = {
        {"two traces, invoked in the other order", "",
         "let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n"
         "let g = trace_hit(frame.worlds[1], r, path_rays.surface, mut p)\n"
         "g.invoke(mut p)\n"
         "h.invoke(mut p)\n"},
        {"an index assigned between the trace and the invocation", "",
         "let mut k = id.x % 4\n"
         "let h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
         "k = (k + 1) % 4\n"
         "h.invoke(mut p)\n"},
        {"a while loop that moves its index before the invocation", "",
         "let mut k = 0\n"
         "while k < 4:\n"
         "    let h = trace_hit(frame.worlds[k], r, path_rays.surface, mut p)\n"
         "    k = k + 1\n"
         "    h.invoke(mut p)\n"},
        {"a loop's branch", "",
         "for i in 0 ..< 4:\n"
         "    if i % 2 == 0:\n"
         "        let h = trace_hit(frame.worlds[i], r, path_rays.surface, mut p)\n"
         "        if h.is_hit:\n"
         "            h.invoke(mut p)\n"},
        {"a callee inlined twice",
         "fun run(q: mut radiance, s: ray, k: int){frame}:\n"
         "    let h = trace_hit(frame.worlds[nonuniform k], s, path_rays.surface, mut q)\n"
         "    h.invoke(mut q)\n",
         "run(mut p, r, id.x % 4)\nrun(mut p, r, (id.x + 1) % 4)\n"},
        {"a callee in a loop",
         "fun run(q: mut radiance, s: ray, k: int){frame}:\n"
         "    let h = trace_hit(frame.worlds[k], s, path_rays.surface, mut q)\n"
         "    h.invoke(mut q)\n",
         "for i in 0 ..< 3:\n    run(mut p, r, i)\n"},
        {"an index a branch set", "",
         "let mut k = 0\n"
         "if id.x > 3:\n"
         "    k = 2\n"
         "let h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
         "h.invoke(mut p)\n"},
    };
    sgl::check::feature_fill const fills[]
        = {{.f = sgl::check::feature::shader_execution_reordering, .how = sgl::check::fill::nvapi}};
    auto const headers = nvapi_headers();
    for (auto const& c : cases)
    {
        auto const source = pinning(c.declarations, c.body);
        auto const of = [&](sgl::emit::native_route route)
        {
            return sgl::compile_to_text({.source = source,
                                         .entry_point = "shoot",
                                         .target = sgl::emit::target::hlsl_dx12,
                                         .device_features = sgl::check::feature::shader_execution_reordering,
                                         .fills = fills,
                                         .native = route});
        };
        // SGL's DXIL passes the validator, and every TraceRay of it takes one handle where the structure is one
        auto const native = of(sgl::emit::native_route::require);
        REQUIRE(native.has_value()).dump("case", c.what).dump("error", native.has_error() ? native.error() : cc::string());
        auto const refused = compiler.value().validate(native.value().dxil);
        CHECK(!refused.has_value()).dump("case", c.what).dump("validator", refused.value_or(cc::string()));
        if (headers.empty())
            continue;
        // the text, its copy of the structure a local, compiles through DXC on NVAPI's header
        auto const text = of(sgl::emit::native_route::text);
        REQUIRE(text.has_value()).dump("case", c.what).dump("error", text.has_error() ? text.error() : cc::string());
        CHECK(text.value().text.contains("RaytracingAccelerationStructure traced_structure"))
            .dump("case", c.what)
            .dump("text", text.value().text);
        auto const theirs = dxc_of(compiler.value(), text.value(), headers);
        CHECK(theirs.has_value())
            .dump("case", c.what)
            .dump("dxc", theirs.has_error() ? theirs.error() : cc::string())
            .dump("text", text.value().text);
    }
    if (headers.empty())
        SKIP("the pinned NVAPI headers are not fetched: uv run extern/nvapi/fetch-nvapi.py");
}

TEST("slib - SGL's DXIL of an nvapi invocation traces through the very handle its trace took (EMIT-263)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    // the index moves between the trace and the invocation, so a structure read again would be another one
    auto const source = pinning("", "let mut k = id.x % 4\n"
                                    "let h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
                                    "k = (k + 1) % 4\n"
                                    "h.invoke(mut p)\n");
    sgl::check::feature_fill const fills[]
        = {{.f = sgl::check::feature::shader_execution_reordering, .how = sgl::check::fill::nvapi}};
    auto const native = sgl::compile_to_text({.source = source,
                                              .entry_point = "shoot",
                                              .target = sgl::emit::target::hlsl_dx12,
                                              .device_features = sgl::check::feature::shader_execution_reordering,
                                              .fills = fills,
                                              .native = sgl::emit::native_route::require});
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    auto const ours = compiler.value().disassemble(native.value().dxil);
    REQUIRE(ours.has_value());
    // each TraceRay's first operand after its opcode, the structure's handle
    auto handles = cc::vector<cc::string>();
    auto const& listing = ours.value();
    for (auto at = listing.find("@dx.op.traceRay."); at >= 0; at = listing.find("@dx.op.traceRay.", at + 1))
    {
        auto const line_end = listing.find('\n', at);
        auto const line = listing.subview({.offset = at, .size = (line_end < 0 ? listing.size() : line_end) - at});
        if (line.contains("declare") || !line.contains("%dx.types.Handle %"))
            continue;
        auto const from = line.find("%dx.types.Handle %") + 17;
        auto const comma = line.find(',', from);
        handles.push_back(cc::string(line.subview({.offset = from, .size = comma - from})));
    }
    REQUIRE(handles.size() == 2).dump("ours", listing);
    CHECK(handles[0] == handles[1]).dump("ours", listing);
}

TEST("slib - an HLSL pixel stage whose path ends in a discard compiles through DXC, for dx12 and vulkan (EMIT-117)")
{
    auto compiler = ssc::dxc::compiler::create();
    if (compiler.has_error())
        SKIP("no DXC");
    struct discard_case
    {
        cc::string_view what;
        cc::string_view source;
        bool returns_zero;
    };
    discard_case const cases[] = {
        {"a float4",
         "struct varyings:\n    @position position: hpos4\n\n"
         "@pixel fun ps(p: varyings) -> float4:\n"
         "    if p.position.x > 2.0:\n"
         "        discard\n"
         "    else:\n"
         "        return float4(1.0, 1.0, 1.0, 1.0)\n",
         true},
        {"an int4",
         "struct varyings:\n    @position position: hpos4\n\n"
         "@pixel fun ps(p: varyings) -> int4:\n"
         "    if p.position.x > 2.0:\n"
         "        discard\n"
         "    else:\n"
         "        return int4(1, 1, 1, 1)\n",
         true},
        {"a target with a depth",
         "struct varyings:\n    @position position: hpos4\n\n"
         "@pixel struct target:\n    color: float4\n    @depth depth: float\n\n"
         "@pixel fun ps(p: varyings) -> target:\n"
         "    if p.position.x > 2.0:\n"
         "        discard\n"
         "    else:\n"
         "        return {color = float4(1.0, 1.0, 1.0, 1.0), depth = 0.5}\n",
         true},
        {"a callee's discard",
         "struct varyings:\n    @position position: hpos4\n\n"
         "fun maybe(x: float):\n"
         "    if x > 2.0:\n"
         "        discard\n\n"
         "@pixel fun ps(p: varyings) -> float4:\n"
         "    maybe(p.position.x)\n"
         "    if p.position.y > 2.0:\n"
         "        return float4(0.0, 1.0, 1.0, 1.0)\n"
         "    else:\n"
         "        return float4(1.0, 1.0, 1.0, 1.0)\n",
         true},
        {"a body that ends in a return",
         "struct varyings:\n    @position position: hpos4\n\n"
         "@pixel fun ps(p: varyings) -> float4:\n"
         "    if p.position.x > 2.0:\n"
         "        discard\n"
         "    return float4(1.0, 1.0, 1.0, 1.0)\n",
         false},
    };
    for (auto const& c : cases)
        for (auto const t : {sgl::emit::target::hlsl_dx12, sgl::emit::target::hlsl_vulkan})
        {
            auto const e = sgl::compile_to_text({.source = c.source, .entry_point = "ps", .target = t});
            REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(e.value().text.contains(")0;\n}") == c.returns_zero).dump("case", c.what).dump("text", e.value().text);
            auto options = ssc::dxc::compile_options();
            if (t == sgl::emit::target::hlsl_vulkan)
                options.target = ssc::dxc::compile_target::spirv;
            for (auto const& arg : e.value().dxc_args)
                options.extra_args.push_back(arg);
            auto const built = compiler.value().compile({.source = e.value().text,
                                                         .entry_point = e.value().entry_point,
                                                         .stage = sg::shader_stage::fragment,
                                                         .model = ssc::dxc::shader_model::sm_6_6},
                                                        options);
            CHECK(built.has_value())
                .dump("case", c.what)
                .dump("dxc", built.has_error() ? built.error().to_string() : cc::string())
                .dump("text", e.value().text);
        }
}
