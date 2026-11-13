#include "emit-test-support.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/macros.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_parallel_for.hh>
#include <nexus/async-test.hh>
#include <nexus/source_root.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

#include <cstdlib>
#include <filesystem>

using namespace cc::primitive_defines;

// SGL's own SPIR-V: what a module holds, and what `spirv-val` says of every compute entry point the fixtures hold.

namespace
{
constexpr cc::string_view k_doubled = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = work.values[id.x] * 2.0 + 1.0\n";

constexpr cc::string_view k_precise = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@float(.precise) @compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    work.values[id.x] = work.values[id.x] * 2.0 + 1.0\n";

constexpr cc::string_view k_rendered = "@vertex struct input:\n"
                                       "    at: float3\n"
                                       "\n"
                                       "@vertex fun run(v: input) -> hpos4 => hpos4(v.at.x, v.at.y, v.at.z, 1.0)\n";

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> written(cc::string_view source,
                                                                  cc::string_view entry,
                                                                  sgl::emit::native_route route)
{
    return sgl::compile_to_text(
        {.source = source, .entry_point = entry, .target = sgl::emit::target::hlsl_vulkan, .native = route});
}

/// How many times the module decorates something with `decoration`, which takes no operands.
[[nodiscard]] isize decorated(cc::span<u32 const> words, u32 decoration)
{
    auto count = isize(0);
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 71 && words[at + 2] == decoration)
            ++count;
    return count;
}

/// How many instructions of `opcode` the module holds.
[[nodiscard]] isize counted(cc::span<u32 const> words, u32 opcode)
{
    auto count = isize(0);
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        count += (words[at] & 0xffffu) == opcode ? 1 : 0;
    return count;
}

/// Whether the module states `capability`.
[[nodiscard]] bool has_capability(cc::span<u32 const> words, u32 capability)
{
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 17 && words[at + 1] == capability)
            return true;
    return false;
}

/// How many times the module states the execution mode `mode`.
[[nodiscard]] isize with_mode(cc::span<u32 const> words, u32 mode)
{
    auto count = isize(0);
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 16 && words[at + 2] == mode)
            ++count;
    return count;
}

constexpr cc::string_view k_pixel_surface
    = "require binding_arrays\n"
      "struct link:\n"
      "    @position position: hpos4\n"
      "    near: float2\n"
      "\n"
      "@pixel struct written:\n"
      "    color: float4\n"
      "    @depth(.greater_equal) depth: float\n"
      "    @sample_mask covered: uint\n"
      "\n"
      "@pixel fun surface_ps(l: link, @is_front_facing front: bool, @sample_mask seen: uint) -> written:\n"
      "    if not front => discard\n"
      "    return { color = float4(ddx(l.near.x), ddy(l.near.y), fwidth(l.near.x), 1.0), depth = 0.5, covered = seen "
      "}\n"
      "\n"
      "binding shelf:\n"
      "    images: texture_2d[float4][2]\n"
      "    smooth: sampler\n"
      "\n"
      "@pixel fun shelf_ps(l: link){shelf} -> float4:\n"
      "    let at = (l.position.x as int) % 2\n"
      "    return shelf.images[nonuniform at].sample(l.near, shelf.smooth)\n";

constexpr cc::string_view k_primitive_stages
    = "require geometry_shader\n"
      "require tessellation_shader\n"
      "struct point:\n"
      "    at: float2\n"
      "    frame: mat2\n"
      "\n"
      "struct factors:\n"
      "    @edge_factors edges: float[3]\n"
      "    @inside_factors inside: float\n"
      "    scale: float\n"
      "\n"
      "struct link:\n"
      "    @position position: hpos4\n"
      "\n"
      "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
      "fun control(patch: point[3]) -> factors:\n"
      "    return { edges = [patch[0].at.x, 2.0, 3.0], inside = 4.0, scale = 1.0 }\n"
      "\n"
      "@tessellation_evaluation\n"
      "fun evaluate(patch: point[3], f: factors, @domain_location uvw: float3) -> link:\n"
      "    return { position = hpos4(uvw.x * patch[0].at.x, f.scale, f.edges[1], 1.0) }\n"
      "\n"
      "@geometry(max_vertices = 3)\n"
      "fun some(tri: link[3], stream: mut triangle_stream[link]):\n"
      "    for i in 0 ..< 3:\n"
      "        if tri[i].position.x > 0.0:\n"
      "            stream.emit(tri[i])\n"
      "    stream.end_strip()\n";

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

[[nodiscard]] cc::string read_all(cc::string_view path)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
}

void write_words(cc::string_view path, cc::span<u32 const> words)
{
    auto adapter = cc::file_write_stream_adapter::create(path);
    REQUIRE(adapter.has_value());
    auto stream = adapter.value().stream();
    REQUIRE(stream.write(words.as_bytes()).has_value());
    REQUIRE(stream.flush().has_value());
}
} // namespace

TEST("sgl spirv - a module is SPIR-V 1.6, and states its workgroup")
{
    auto const e = written(k_doubled, "run", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const& words = e.value().spirv;
    REQUIRE(words.size() > 5);
    CHECK(words[0] == 0x07230203u);
    CHECK(words[1] == 0x00010600u);
    // a reader of the program sees what the words were written from
    CHECK(e.value().text.contains("entry run [64 1 1]"));
    CHECK(e.value().text.contains("fmul"));
}

TEST("sgl spirv - an entry point's guarantee stands on each float operation it keeps (EMIT-185)")
{
    auto const fast = written(k_doubled, "run", sgl::emit::native_route::require);
    auto const precise = written(k_precise, "run", sgl::emit::native_route::require);
    REQUIRE(fast.has_value());
    REQUIRE(precise.has_value());
    CHECK(decorated(fast.value().spirv, 42) == 0);
    CHECK(decorated(precise.value().spirv, 42) == 2);
}

TEST("sgl spirv - a vector scaled by a scalar builds no splat, and an if around little is a select")
{
    auto const e = written("binding work:\n"
                           "    values: mut buffer[float4]\n"
                           "\n"
                           "@compute(64) fun run(@thread_id id: int3){work}:\n"
                           "    let s = work.values[0].w\n"
                           "    let mut v = work.values[id.x] * s\n"
                           "    if v.x > 1.0:\n"
                           "        v = v + float4(1.0, 2.0, 3.0, 4.0)\n"
                           "    work.values[id.x] = v\n",
                           "run", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const& words = e.value().spirv;
    CHECK(counted(words, 142) == 1); // OpVectorTimesScalar
    CHECK(counted(words, 133) == 0); // no OpFMul of a splat
    CHECK(counted(words, 80) == 0);  // nor the OpCompositeConstruct it read
    CHECK(counted(words, 169) == 1); // OpSelect
    CHECK(counted(words, 245) == 0); // and no OpPhi
    CHECK(counted(words, 247) == 0); // nor the OpSelectionMerge it folded
}

TEST("sgl spirv - a pixel stage's surface is SPIR-V's own, and a discard goes on as a helper (EMIT-238)")
{
    auto const e = written(k_pixel_surface, "surface_ps", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const& words = e.value().spirv;
    CHECK(counted(words, 5380) == 1); // OpDemoteToHelperInvocation
    CHECK(counted(words, 252) == 0);  // never OpKill, which would take the neighbour's derivatives with it
    CHECK(counted(words, 207) == 1);  // OpDPdx
    CHECK(counted(words, 208) == 1);  // OpDPdy
    CHECK(counted(words, 209) == 1);  // OpFwidth
    CHECK(has_capability(words, 5379));
    CHECK(with_mode(words, 12) == 1); // DepthReplacing
    CHECK(with_mode(words, 14) == 1); // DepthGreater
    // `SampleMask` is an array of one, on both sides of the stage
    CHECK(counted(words, 65) >= 2);
}

TEST("sgl spirv - a nonuniform index reaches each value it decides (EMIT-239)")
{
    auto const e = written(k_pixel_surface, "shelf_ps", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const& words = e.value().spirv;
    CHECK(has_capability(words, 5301));
    CHECK(has_capability(words, 5307));
    // the index, the chain, the load and the sampled image
    CHECK(decorated(words, 5300) == 4);
}

TEST("sgl spirv - what has no lowering yet is named, and a preference falls back to the text")
{
    auto const required = written(k_rendered, "run", sgl::emit::native_route::require);
    REQUIRE(required.has_error());
    CHECK(required.error().contains("which the SPIR-V route does not lower yet"));

    auto const preferred = written(k_rendered, "run", sgl::emit::native_route::prefer);
    REQUIRE(preferred.has_value());
    CHECK(preferred.value().spirv.empty());
    CHECK(preferred.value().native_gap.contains("no struct"));
    CHECK(!preferred.value().text.empty());
}

TEST("sgl spirv - a control stage hands each point on, waits for them all, then runs its body once per patch")
{
    auto const e = written(k_primitive_stages, "control", sgl::emit::native_route::require);
    REQUIRE(e.has_value());
    auto const words = cc::span<u32 const>(e.value().spirv);
    CHECK(has_capability(words, 3)); // Tessellation
    CHECK(!has_capability(words, 2));
    CHECK(counted(words, 224) == 1);  // OpControlBarrier
    CHECK(counted(words, 57) == 1);   // OpFunctionCall
    CHECK(counted(words, 54) == 2);   // OpFunction: the entry point and the body
    CHECK(with_mode(words, 22) == 1); // Triangles
    CHECK(with_mode(words, 4) == 1);  // VertexOrderCw: EMIT-134 mirrors the `.counter_clockwise` the source names
    CHECK(with_mode(words, 26) == 1); // OutputVertices
    CHECK(decorated(words, 15) == 3); // Patch: both factor arrays and `scale`
}

TEST("sgl spirv - an evaluation stage reads patch constants as inputs, and a geometry stage's emit stays in its branch")
{
    auto const evaluated = written(k_primitive_stages, "evaluate", sgl::emit::native_route::require);
    REQUIRE(evaluated.has_value());
    auto const read = cc::span<u32 const>(evaluated.value().spirv);
    CHECK(with_mode(read, 22) == 1);
    CHECK(with_mode(read, 26) == 0);
    CHECK(decorated(read, 15) == 3);

    auto const emitted = written(k_primitive_stages, "some", sgl::emit::native_route::require);
    REQUIRE(emitted.has_value());
    auto const stream = cc::span<u32 const>(emitted.value().spirv);
    CHECK(has_capability(stream, 2));  // Geometry
    CHECK(with_mode(stream, 29) == 1); // OutputTriangleStrip
    CHECK(counted(stream, 218) == 1);  // OpEmitVertex
    CHECK(counted(stream, 219) == 1);  // OpEndPrimitive
    CHECK(counted(stream, 247) >= 1);  // the branch around it: nothing lifts an effect out
}

TEST("sgl spirv - a patch's links take the locations after the control point's pieces")
{
    auto const e = written(k_primitive_stages, "control", sgl::emit::native_route::text);
    REQUIRE(e.has_value());
    // `at` at 0, `frame`'s two columns at 1 and 2
    CHECK(e.value().text.contains("[[vk::location(3)]] float scale"));
}

// A task per file, since each module is a spirv-val process, and those one after another were the binary's critical path.
ASYNC_TEST("sgl spirv - every entry point of the fixtures that lowers is what spirv-val calls valid")
{
#if defined(CC_OS_EMSCRIPTEN) || defined(CC_OS_IOS) || defined(CC_OS_TVOS)
    SKIP("this platform runs no spirv-val, and has no std::system to start one with");
    co_return;
#else
    // dev.py puts the pinned Vulkan SDK's tools on the path
#if defined(CC_OS_WINDOWS)
    constexpr auto probe = "spirv-val --version > NUL 2>&1";
#else
    constexpr auto probe = "spirv-val --version > /dev/null 2>&1";
#endif
    if (std::system(probe) != 0)
        SKIP("no spirv-val on the path");

    auto files = files_under(nx::source_path(SGL_SG_SHADERS_DIR));
    files.push_back_range(files_under(cc::format("{}/../sgl_mixed", nx::source_path(SGL_SG_SHADERS_DIR))));
    files.push_back_range(files_under(nx::source_path(SGL_SR_SHADERS_DIR)));
    REQUIRE(!files.empty());

    struct lowered_counts
    {
        int compute = 0;
        int raster = 0;
        int primitive = 0;
        int mesh = 0;
        int ray = 0;
        int recording = 0;
        int guarded = 0;
    };
    auto counts = cc::vector<lowered_counts>::create_defaulted(files.size());
    co_await cc::async_parallel_for(
        files.size(),
        [&](isize file_index)
        {
            auto const& file = files[file_index];
            auto& n = counts[file_index];
            auto const module_path = cc::format("{}/sgl-spirv-emit-test-{}.spv", cc::temp_directory_path(), file_index);
            auto const validates = [&](cc::span<u32 const> words)
            {
                write_words(module_path, words);
                auto command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
                return std::system(command.c_str_materialize()) == 0;
            };
            // every eighth module is sampled, counted from the file's index so a file of fewer still has its turn
            auto lined_modules = file_index;

            auto const source = read_all(file);
            // what names a module of another directory is the module tests', and this one reads a file alone
            auto const all = sgl::compile_all_to_text({.source = source, .targets = {sgl::emit::target::hlsl_vulkan}});
            if (all.has_error())
                return;
            for (auto const& entry : all.value())
            {
                auto const e = sgl::compile_to_text(
                    {.source = source,
                     .entry_point = entry.entry_point,
                     .target = sgl::emit::target::hlsl_vulkan,
                     .diagnostics = true,
                     .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                     .native = sgl::emit::native_route::prefer});
                if (e.has_error() || e.value().spirv.empty())
                    continue;
                auto const stage = e.value().stage;
                auto const is_raster = stage == sgl::check::stage::vertex || stage == sgl::check::stage::pixel;
                auto const is_primitive = stage == sgl::check::stage::geometry
                                       || stage == sgl::check::stage::tessellation_control
                                       || stage == sgl::check::stage::tessellation_evaluation;
                auto const is_mesh = stage == sgl::check::stage::mesh || stage == sgl::check::stage::task;
                auto const is_ray = stage >= sgl::check::stage::raygen;
                ++(is_ray ? n.ray : is_mesh ? n.mesh : is_primitive ? n.primitive : is_raster ? n.raster : n.compute);
                n.recording += e.value().debug_sites.empty() ? 0 : 1;
                CHECK(validates(e.value().spirv)).dump("file", file).dump("entry", entry.entry_point);

                // EMIT-257: the line table is SPIR-V spirv-val takes too, every eighth module, and each under --thorough
                if (nx::is_thorough() || lined_modules++ % 8 == 0)
                {
                    auto const lined = sgl::compile_to_text(
                        {.source = source,
                         .source_name = file,
                         .entry_point = entry.entry_point,
                         .target = sgl::emit::target::hlsl_vulkan,
                         .diagnostics = true,
                         .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                         .native = sgl::emit::native_route::prefer,
                         .native_line_table = true});
                    REQUIRE(lined.has_value());
                    CHECK(validates(lined.value().spirv))
                        .dump("file", file)
                        .dump("entry", entry.entry_point)
                        .dump("with", "its line table");
                }

                // each profile, nothing here running what it guards; a report writes through the debug buffer
                for (auto const guard : {sgl::check::undefined_guard::abort, sgl::check::undefined_guard::report,
                                         sgl::check::undefined_guard::define})
                {
                    auto const profile = sgl::compile_to_text(
                        {.source = source,
                         .entry_point = entry.entry_point,
                         .target = sgl::emit::target::hlsl_vulkan,
                         .diagnostics = true,
                         .guards_undefined = guard,
                         .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
                         .native = sgl::emit::native_route::prefer});
                    if (profile.has_error() || profile.value().spirv.empty())
                        continue;
                    ++n.guarded;
                    // a process per module, so a default run validates every eighth and --thorough takes them all
                    if (!nx::is_thorough() && (file_index + n.guarded) % 8 != 0)
                        continue;
                    CHECK(validates(profile.value().spirv))
                        .dump("file", file)
                        .dump("entry", entry.entry_point)
                        .dump("guard", int(guard));
                }
            }
        },
        {.grain = 1});

    auto lowered = 0;
    auto raster_lowered = 0;
    auto primitive_lowered = 0;
    auto mesh_lowered = 0;
    auto ray_lowered = 0;
    auto recording = 0;
    auto guarded = 0;
    for (auto const& n : counts)
    {
        lowered += n.compute;
        raster_lowered += n.raster;
        primitive_lowered += n.primitive;
        mesh_lowered += n.mesh;
        ray_lowered += n.ray;
        recording += n.recording;
        guarded += n.guarded;
    }

    auto const module_path = cc::format("{}/sgl-spirv-emit-test.spv", cc::temp_directory_path());
    auto command = cc::format("spirv-val --target-env vulkan1.3 --scalar-block-layout \"{}\"", module_path);
    // what no fixture prints: every family a record holds, and a vertex stage's
    constexpr cc::string_view records
        = "require shader_f16\n"
          "enum tone:\n"
          "    low\n"
          "    high\n"
          "\n"
          "struct link:\n"
          "    @position position: hpos4\n"
          "\n"
          "@compute(4) fun records_cs(@thread_id id: int3):\n"
          "    print \"$(id.x) $(id.x as uint) $(id.x as float) $(half(0.5)) $(id.x > 1) $id $(tone.high)\"\n"
          "\n"
          "@vertex fun records_vs(@vertex_index v: int) -> link:\n"
          "    print \"vertex $v\"\n"
          "    return { position = hpos4(0.0, 0.0, 0.0, 1.0) }\n";
    for (auto const entry : {"records_cs", "records_vs"})
    {
        auto const e = sgl::compile_to_text(
            {.source = records,
             .entry_point = entry,
             .target = sgl::emit::target::hlsl_vulkan,
             .diagnostics = true,
             .device_features = sgl::check::feature_set(sgl::check::feature::vertex_pipeline_stores),
             .native = sgl::emit::native_route::require});
        REQUIRE(e.has_value());
        CHECK(e.value().debug_sites.size() == 1);
        write_words(module_path, e.value().spirv);
        CHECK(std::system(command.c_str_materialize()) == 0).dump("entry", entry);
    }

    // the numbers the slice reaches, so a lowering lost shows here rather than as a quiet fall back to DXC
    CC_LOG_INFO("SGL's SPIR-V lowers {} compute entry points, {} vertex and pixel ones, {} geometry and tessellation "
                "ones, and {} mesh and task ones",
                lowered, raster_lowered, primitive_lowered, mesh_lowered);
    CC_LOG_INFO("and {} ray-tracing ones", ray_lowered);
    CC_LOG_INFO("{} of them write debug records, and the robustness profiles lower {} entry points", recording, guarded);
    CHECK(lowered >= 120);
    CHECK(raster_lowered >= 82);
    CHECK(ray_lowered >= 48);
    CHECK(primitive_lowered >= 18);
    CHECK(recording >= 2);
    CHECK(guarded >= 880);
    CHECK(mesh_lowered >= 13);
#endif
}
