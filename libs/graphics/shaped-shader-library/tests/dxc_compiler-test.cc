#include <shaped-shader-library/compiler/dxc_compiler.hh>

#if SLIB_HAS_DXC

#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <slib_test_shaders.hh>

#include <memory>

// The one place slib meets a real compiler. Everything else in the library is covered with a fake one so
// it runs on every platform; these run only where DXC exists, and check that the real thing lines up with
// the seam — that the package's HLSL actually builds, includes and all.

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
    (void)cc::try_async_blocking_get_singlethreaded(shader);

    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}
} // namespace

TEST("slib - dxc compiler advertises the hlsl -> dxil edge")
{
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());

    CHECK(compiler.value()->source_language() == slib::shader_language::hlsl);
    CHECK(compiler.value()->target_format() == sg::shader_format::dxil);
}

TEST("slib - dxc compiles the generated package's compute shader")
{
    slib::shader_library lib;
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));

    lib.add_package(slib_test::shaders::package());

    auto const& shader = await(slib_test::shaders::invert.compute.main->acquire(sg::shader_format::dxil));

    CHECK(shader.stage == sg::shader_stage::compute);
    CHECK(shader.format == sg::shader_format::dxil);
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

TEST("slib - dxc compiles both entry points of one file")
{
    slib::shader_library lib;
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib_test::shaders::package());

    auto const& vs = await(slib_test::shaders::blit.vertex.main_vs->acquire(sg::shader_format::dxil));
    auto const& ps = await(slib_test::shaders::blit.fragment.main_ps->acquire(sg::shader_format::dxil));

    CHECK(vs.stage == sg::shader_stage::vertex);
    CHECK(ps.stage == sg::shader_stage::fragment);
    CHECK(vs.entry_point == "main_vs");
    CHECK(ps.entry_point == "main_ps");
    CHECK(vs.bytecode.size() > 0);
    CHECK(ps.bytecode.size() > 0);
}

TEST("slib - dxc reports a broken shader on the async channel")
{
    slib::shader_asset_handle broken;
    slib::shader_definition definitions[] = {
        {.path = "broken.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &broken},
    };

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("broken.hlsl", "this is not HLSL at all");

    slib::shader_library lib;
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib::shader_package{.name = "broken_pkg", .definitions = definitions}, fs);

    // A shader that does not build must not throw or abort — it is an error a caller handles.
    auto const shader = broken->acquire(sg::shader_format::dxil);
    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get_singlethreaded(shader);
    CHECK(shader->has_error());
}

TEST("slib - dxc hot-reloads a real shader")
{
    // The whole stack against the real compiler: an edit, a scan, a recompile, a new shader. Unthreaded
    // and in memory, so it is deterministic rather than a sleep-and-hope.
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
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(slib::shader_package{.name = "reload_pkg", .definitions = definitions}, fs);

    CHECK(await(asset->acquire(sg::shader_format::dxil)).workgroup_size.value().x == 8);
    lib.start_hot_reload({.unthreaded = true});

    fs->write("cs.hlsl", shader_with_group_size(16));
    lib.poll_hot_reload();

    CHECK(await(asset->acquire(sg::shader_format::dxil)).workgroup_size.value().x == 16);
    CHECK(asset->generation() == 1);
}

#endif
