#include <shaped-shader-library/compiler/dxc_compiler.hh>

#if SLIB_HAS_DXC

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
// The prelude's whole surface in one compute shader, so a target that rejects any part of it fails here.
constexpr char const* k_prelude_shader = R"(
#include "sc/portable.hlsli"

struct spike_constants { uint scale; };
SC_INLINE_CONSTANTS(spike_constants, Push);

#define SC_GROUP 0
SC_BINDING Texture2D<float4> Albedo;
SC_BINDING SamplerState LinearSampler;
#undef SC_GROUP

#define SC_GROUP 1
SC_BINDING RWTexture2D<float4> Output;
#undef SC_GROUP

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 uv = (float2(tid.xy) + 0.5f) / 64.0f;
    Output[tid.xy] = Albedo.SampleLevel(LinearSampler, uv, 0) * Push.scale;
}
)";
} // namespace

TEST("slib - the portable prelude is mounted and needs no wiring", exclusive("slib-shader-library"))
{
    // No mount() call anywhere: `sc` is there because a shader_library exists.
    slib::shader_library lib;
    auto compiler = make_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));

    auto const shader = lib.compile_source(k_prelude_shader, sg::shader_stage::compute, "main", k_target_format);
    auto const& compiled = await(shader);

    // Three bindings plus the inline constants, whatever the target spells them as.
    CHECK(compiled.bindings.size() == 4);
    CHECK(compiled.workgroup_size.value().x == 8);
}

TEST("slib - one prelude source reflects the same names and kinds on both targets", exclusive("slib-shader-library"))
{
    // This is the equivalence a package-wide check would assert; here it covers the prelude itself.
    // Indices are deliberately not compared — SPIR-V takes the counter's number and DXIL takes DXC's own assignment.
    slib::shader_library lib;
    auto dxil = slib::create_dxc_compiler();
    auto spirv = slib::create_dxc_spirv_compiler();
    REQUIRE(dxil.has_value());
    REQUIRE(spirv.has_value());
    lib.add_compiler(cc::move(dxil.value()));
    lib.add_compiler(cc::move(spirv.value()));

    auto const as_dxil = lib.compile_source(k_prelude_shader, sg::shader_stage::compute, "main", sg::shader_format::dxil);
    auto const as_spirv
        = lib.compile_source(k_prelude_shader, sg::shader_stage::compute, "main", sg::shader_format::spirv);
    auto const& a = await(as_dxil);
    auto const& b = await(as_spirv);

    REQUIRE(a.bindings.size() == b.bindings.size());
    for (auto const& binding : b.bindings)
    {
        sg::binding const* other = nullptr;
        for (auto const& candidate : a.bindings)
            if (candidate.name == binding.name)
                other = &candidate;

        REQUIRE(other != nullptr);
        CHECK(other->type == binding.type);
    }
}

#endif
