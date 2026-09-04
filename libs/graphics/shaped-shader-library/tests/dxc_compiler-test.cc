#include <shaped-shader-library/compiler/dxc_compiler.hh>

#if SLIB_HAS_DXC

#include <clean-core/common/log.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_shaders.hh>

#include <memory>

// The one place slib meets a real compiler.
// Everything else is covered with a fake one so it runs on every platform; these run only where DXC exists, and check that the real thing lines up with the seam.

namespace
{
/// Drives a compile to completion and returns it.
///
/// acquire() hands back a cold cc::async node — a real app installs a default async pool and its workers
/// run it; with none installed the caller drives, which is what these tests do (and what ssc's own tests
/// do). Driving an already-finished node is a no-op, so a shader the watcher already built passes
/// straight through.
sg::compiled_shader const& await(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);

    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}

// Which bytecode this build can actually produce end to end.
// DXIL reflection reads a container beside the bytecode through the Windows SDK's d3d12shader.h, which the Linux DXC
// release does not ship — so these tests exercise the platform's own format rather than asserting one of them.
// What is under test is the slib adapter, not either bytecode format.
#ifdef CC_OS_WINDOWS
constexpr auto k_target_format = sg::shader_format::dxil;
[[nodiscard]] auto make_dxc_compiler()
{
    return slib::create_dxc_compiler();
}
#else
constexpr auto k_target_format = sg::shader_format::spirv;
[[nodiscard]] auto make_dxc_compiler()
{
    return slib::create_dxc_spirv_compiler();
}
#endif
} // namespace

TEST("slib - dxc compiler advertises its hlsl -> bytecode edge", exclusive("slib-shader-library"))
{
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());

    CHECK(compiler.value()->source_language() == slib::shader_language::hlsl);
    CHECK(compiler.value()->target_format() == k_target_format);
}

TEST("slib - dxc compiles the generated package's compute shader", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));

    lib.add_package(slib_test::shaders::package());

    auto const& shader = await(slib_test::shaders::invert.compute.main->acquire(k_target_format));

    CHECK(shader.stage == sg::shader_stage::compute);
    CHECK(shader.format == k_target_format);
    CHECK(shader.entry_point == "main");
    CHECK(shader.bytecode.size() > 0);
    CHECK(shader.compiler.name == "dxc");

    // The include carried SLIB_TEST_GROUP_SIZE, so reflecting it back proves the resolver reached the
    // .hlsli through the mount rather than DXC finding it on disk by luck.
    REQUIRE(shader.workgroup_size.has_value());
    CHECK(shader.workgroup_size.value().x == 64);

    // Reflection came back with the shader: this is what a pipeline is built from.
    CHECK(shader.bindings.size() == 1);
    CHECK(shader.bindings[0].name == "gOutput");
}

TEST("slib - dxc compiles both entry points of one file", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib_test::shaders::package());

    auto const& vs = await(slib_test::shaders::blit.vertex.main_vs->acquire(k_target_format));
    auto const& ps = await(slib_test::shaders::blit.fragment.main_ps->acquire(k_target_format));

    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(ps.stage == sg::shader_stage::fragment);
    CHECK(vs.entry_point == "main_vs");
    CHECK(ps.entry_point == "main_ps");
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);
}

TEST("slib - dxc reports a broken shader on the async channel", exclusive("slib-shader-library"))
{
    slib::shader_asset_handle broken;
    slib::shader_definition definitions[] = {
        {.path = "broken.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &broken},
    };

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("broken.hlsl", "this is not HLSL at all");

    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib::shader_package{.name = "broken_pkg", .definitions = definitions}, fs);

    // A shader that does not build must not throw or abort — it is an error a caller handles.
    auto const shader = broken->acquire(k_target_format);
    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);
    CHECK(shader->has_error());
}

TEST("slib - dxc hot-reloads a real shader", exclusive("slib-shader-library"))
{
    // The whole stack against the real compiler: an edit, a scan, a recompile, a new shader.
    // Unthreaded and in memory, so it is deterministic rather than a sleep-and-hope.
    slib::shader_asset_handle asset;
    slib::shader_definition definitions[] = {
        {.path = "cs.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &asset},
    };

    auto const shader_with_group_size = [](int size)
    {
        return cc::format("RWStructuredBuffer<float> gOut : register(u0);\n"
                          "[numthreads({}, 1, 1)] void main(uint3 t : SV_DispatchThreadID) {{ gOut[t.x] = 1; }}\n",
                          size);
    };

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("cs.hlsl", shader_with_group_size(8));

    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib::shader_package{.name = "reload_pkg", .definitions = definitions}, fs);

    CHECK(await(asset->acquire(k_target_format)).workgroup_size.value().x == 8);
    lib.start_hot_reload({.unthreaded = true});

    fs->write("cs.hlsl", shader_with_group_size(16));
    lib.poll_hot_reload();

    CHECK(await(asset->acquire(k_target_format)).workgroup_size.value().x == 16);
    CHECK(asset->generation() == 1);
}

namespace
{
// Q8's shape, declared as an annotated namespace instead of numbered by DXC.
// `pixel_only` is declared FIRST and read only by the pixel stage, so without a written address the vertex stage
// takes t0 for `both` while the pixel stage takes t1 -- one resource, one name, two addresses, from one source
// that named neither.
constexpr char const* k_two_stage_shader = R"(
#pragma sc group 0
namespace frame_bindings
{
    Texture2D<float4> pixel_only;
    Texture2D<float4> both;
    SamplerState samp;
}

struct vs_out { float4 pos : SV_Position; float2 uv : TEXCOORD; };

vs_out main_vs(uint id : SV_VertexID)
{
    vs_out o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * 2.0f - 1.0f, frame_bindings::both.SampleLevel(frame_bindings::samp, o.uv, 0).x, 1);
    return o;
}

float4 main_ps(vs_out i) : SV_Target
{
    return frame_bindings::both.SampleLevel(frame_bindings::samp, i.uv, 0)
         + frame_bindings::pixel_only.SampleLevel(frame_bindings::samp, i.uv, 0);
}
)";

[[nodiscard]] sg::binding const* find_binding(sg::compiled_shader const& shader, cc::string_view name)
{
    for (auto const& binding : shader.bindings)
        if (binding.name == name)
            return &binding;
    return nullptr;
}
} // namespace

#ifdef CC_OS_WINDOWS

TEST("slib - one annotated source gives every stage and target the same address", exclusive("slib-shader-library"))
{
    // The test Q8 could not pass: a shared binding keeps its address across the stages of one pipeline, and
    // across the two targets, because the rewrite wrote it into the source.
    slib::shader_library lib;
    auto dxil = slib::create_dxc_compiler();
    auto spirv = slib::create_dxc_spirv_compiler();
    REQUIRE(dxil.has_value());
    REQUIRE(spirv.has_value());
    lib.add_compiler(cc::move(dxil.value()));
    lib.add_compiler(cc::move(spirv.value()));

    for (auto const format : {sg::shader_format::dxil, sg::shader_format::spirv})
    {
        auto const vs = lib.compile_source(k_two_stage_shader, sg::shader_stage::vertex, "main_vs", format);
        auto const ps = lib.compile_source(k_two_stage_shader, sg::shader_stage::fragment, "main_ps", format);

        // Reflection reports the bare name, so `frame_bindings::both` arrives as `both` -- Q10.
        auto const* both_vs = find_binding(await(vs), "both");
        auto const* both_ps = find_binding(await(ps), "both");
        REQUIRE(both_vs != nullptr);
        REQUIRE(both_ps != nullptr);

        // The second declaration in its group, whichever stage referenced what.
        CHECK(both_vs->index == 1);
        CHECK(both_ps->index == 1);

        // And the sampler shares the group's counter rather than starting its own class at zero.
        auto const* samp = find_binding(await(ps), "samp");
        REQUIRE(samp != nullptr);
        CHECK(samp->index == 2);
    }
}

TEST("slib - the group number reaches DXIL as a space and SPIR-V as a set", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto dxil = slib::create_dxc_compiler();
    auto spirv = slib::create_dxc_spirv_compiler();
    REQUIRE(dxil.has_value());
    REQUIRE(spirv.has_value());
    lib.add_compiler(cc::move(dxil.value()));
    lib.add_compiler(cc::move(spirv.value()));

    auto const as_dxil
        = lib.compile_source(k_two_stage_shader, sg::shader_stage::fragment, "main_ps", sg::shader_format::dxil);
    auto const as_spirv
        = lib.compile_source(k_two_stage_shader, sg::shader_stage::fragment, "main_ps", sg::shader_format::spirv);

    auto const* dxil_both = find_binding(await(as_dxil), "both");
    auto const* spirv_both = find_binding(await(as_spirv), "both");
    REQUIRE(dxil_both != nullptr);
    REQUIRE(spirv_both != nullptr);

    // The two ways a shading language namespaces an address, from one number written once.
    REQUIRE(dxil_both->space.has_value());
    CHECK(dxil_both->space.value() == 0);
    REQUIRE(spirv_both->group_index.has_value());
    CHECK(spirv_both->group_index.value() == 0);
}

TEST("slib - an attribute the pass cannot honour fails the compile", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));

    // The failure a macro prelude could not produce: the marker is a comment, so nothing but the pass can notice it.
    constexpr char const* k_bad = R"(
#pragma sc gruop 0
namespace frame
{
    Texture2D<float4> albedo;
}
[numthreads(1, 1, 1)]
void main() {}
)";

    auto const shader = lib.compile_source(k_bad, sg::shader_stage::compute, "main", k_target_format);
    (void)cc::try_async_blocking_get(shader);
    REQUIRE(shader->has_error());
    CHECK(shader->try_error()->underlying().to_string().contains("is not an attribute this pass knows"));
}

#endif

TEST("slib - a compiled shader reflects the addresses its generated table declares", exclusive("slib-shader-library"))
{
    // The third leg of the design's validation, and the only one that asks DXC rather than another parser:
    // the constant table the generator produced, against what the compiler actually built.
    //
    // shade.hlsl reaches the group through the header that declares it, so this also covers the rewrite
    // running on a flattened translation unit rather than on one file.
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib_test::shaders::package());

    auto const& compiled = await(slib_test::shaders::shade.compute.main->acquire(k_target_format));

    // One-directional: reflection reports only what the entry point referenced, so its set is a subset of
    // the declared table.
    // shade.hlsl reads all three.
    for (auto const& declared : slib_test::shaders::frame_bindings::group::declared_bindings())
    {
        auto const* reflected = find_binding(compiled, declared.name);
        if (reflected == nullptr)
        {
            CC_LOG_ERROR("[package] '{}' is declared but was not reflected", declared.name);
            CHECK(false);
            continue;
        }

        CHECK(reflected->index == declared.index);
        CHECK(reflected->count == declared.count);
        CHECK(reflected->type == declared.type);
        CHECK(reflected->texture_dimension == declared.texture_dimension);
    }
}

TEST("slib - an inline-constants block reflects at b0 in the space it named", exclusive("slib-shader-library"))
{
    // Its register is always b0, so a shader that shares a space with a group's `b` registers is exactly the
    // collision stating a space prevents -- and here the group is in space 0 while the block is in space 9.
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib_test::shaders::package());

    auto const& compiled = await(slib_test::shaders::shade.compute.main->acquire(k_target_format));

    auto const* constants = find_binding(compiled, "gConstants");
    REQUIRE(constants != nullptr);
    CHECK(constants->type == sg::binding_type::uniform_buffer);
    CHECK(constants->index == 0);
    REQUIRE(constants->space.has_value());
    CHECK(constants->space.value() == 9);

    // block_size keeps coming from reflection, which is how a routine reads it today.
    //
    // And this is the check that closes the loop on the generated mirror: the size DXC computed for the block,
    // against the size the generator computed for the C++ struct a caller fills.
    // The corpus proves the two halves of the pass agree with each other; only this compares either of them
    // with the compiler.
    REQUIRE(constants->block_size.has_value());
    CHECK(constants->block_size.value() == cc::isize(sizeof(slib_test::shaders::shade_constants)));
}

TEST("slib - a two-slot vertex input compiles to SPIR-V", exclusive("slib-shader-library"))
{
    // mesh.hlsl feeds one entry point from two annotated structs, which is what an instanced draw looks like.
    // A location counter that restarted per struct would put two inputs at location 0, and DXC's validator
    // rejects the module rather than picking one — so this compiling at all is the property under test.
    //
    // SPIR-V explicitly, not k_target_format: the DXIL arm writes no location, so it could never see this.
    slib::shader_library lib;
    auto compiler = slib::create_dxc_spirv_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib_test::shaders::package());

    auto const& vs = await(slib_test::shaders::mesh.vertex.main_vs->acquire(sg::shader_format::spirv));
    CHECK(vs.bytecode.size() > 0);
}

#endif
