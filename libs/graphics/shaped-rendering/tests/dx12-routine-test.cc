#include "fake_routines.hh"

#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-rendering/render_routine.hh>
#include <shaped-rendering/render_routine_library.hh>
#include <shaped-rendering/render_routine_package.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// This test target declares the package itself (see sc_add_shader_package in the CMakeLists); generated
// into the build dir and private to this binary.
#include <sr_test_shaders.hh>

// Context-driven tests, on a dx12 WARP context (software, present on any Windows host, so they run on
// headless CI). WARP + DXC are Windows-only, which is why this whole file is gated on the dx12 backend
// and a shader compiler in the CMakeLists. Two things are proven here:
//   1. the framework's phase orchestration re-runs declare/materialize on a shader reload, but not init_once, and
//   2. a real, test-local routine compiles a compute shader through slib and dispatches it end to end.

namespace
{
// A test-local render routine: fills a caller-provided buffer with the pattern the shader computes.
// Declared entirely in this test binary — the point is that routines need not live in shaped-rendering.
class pattern_fill_routine : public sr::render_routine
{
public:
    static void execute(sr::routine_handle<pattern_fill_routine> const& handle,
                        sg::command_list& cmd,
                        sg::buffer<sg::u32> const& out)
    {
        auto const& self = handle.acquire(cmd);
        CC_ASSERT(self._pipeline != nullptr, "pattern_fill routine failed to initialize");

        sg::named_view const view{.name = "gValues", .view = out.as_readwrite_buffer()};
        auto const group
            = cmd.context().transient.create_binding_group(self._group_layout, cc::span<sg::named_view const>(&view, 1));

        cmd.compute.bind_pipeline(*self._pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(static_cast<int>(out.element_count()));
    }

protected:
    void init_declare(sg::context& ctx) override
    {
        auto const shader = sr::test::shaders::pattern_fill.compute.main->acquire(ctx);
        (void)cc::try_async_blocking_get_singlethreaded(shader); // no async pool here, so drive it inline
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            return; // the context cannot produce a format we can use; execute() then asserts

        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout
            = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {_group_layout}});
        auto const pso = ctx.cached.acquire_compute_pipeline(
            sg::compute_pipeline_description{.shader = *compiled, .layout = layout});
        _pipeline = cc::async_blocking_get_singlethreaded(pso);
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::compute_pipeline_handle _pipeline;
};

struct pattern_ops_package : sr::render_routine_package
{
    sr::routine_handle<pattern_fill_routine> pattern_fill;

protected:
    void setup() override { pattern_fill = register_routine<pattern_fill_routine>(); }
};

// A dx12 WARP context, or nullptr where none is available (the caller SKIPs).
sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sr - routine phases run once, then re-run declare + materialize on a shader reload")
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    // A fresh package (not the shared singleton) so the routine's phase counters start clean.
    auto const pkg = sr::make_package<sr_test::leaf_package>();
    auto const routine = pkg->routine.shared();

    sr::render_routine_library lib;
    lib.add_package(pkg);

    auto cmd = ctx->create_command_list();
    lib.ensure_all_initialized(*cmd);
    CHECK(routine->once == 1);
    CHECK(routine->declare == 1);
    CHECK(routine->materialize == 1);

    // A second pass at the same generation changes nothing.
    lib.ensure_all_initialized(*cmd);
    CHECK(routine->once == 1);
    CHECK(routine->declare == 1);
    CHECK(routine->materialize == 1);

    // A reload bumps the global generation: declare + materialize re-run, init_once does not.
    slib::shader_library().note_reload();
    lib.ensure_all_initialized(*cmd);
    CHECK(routine->once == 1);
    CHECK(routine->declare == 2);
    CHECK(routine->materialize == 2);

    ctx->drop_command_list(cc::move(cmd));
}

TEST("sr - a test-local routine compiles a shader and dispatches it end to end")
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");
    if (!ctx->accepts_shader_format(sg::shader_format::dxil))
        SKIP("context does not accept DXIL");

    auto const shader_lib = std::make_shared<slib::shader_library>();
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    shader_lib->add_compiler(cc::move(compiler.value()));
    shader_lib->add_package(sr::test::shaders::package());

    auto const pkg = sr::make_package<pattern_ops_package>();
    sr::render_routine_library lib;
    lib.add_package(pkg);

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup
    auto const out
        = ctx->persistent.create_buffer<sg::u32>(count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out.raw() != nullptr);

    auto disp = ctx->create_command_list();
    pattern_fill_routine::execute(pkg->pattern_fill, *disp, out);
    ctx->submit_command_list(cc::move(disp));

    // Read the result back in a second list (the buffer decays to COMMON between submits).
    auto down = ctx->create_command_list();
    auto const future = down->download.data_from_buffer<sg::u32>(out.raw(), 0, count);
    ctx->submit_command_list(cc::move(down));

    auto const data = ctx->wait_for(future);
    REQUIRE(data.has_value());
    REQUIRE(data.value().size() == cc::isize(count));
    bool ok = true;
    for (int i = 0; i < count; ++i)
        if (data.value()[i] != cc::u32(i) * 3u + 7u)
            ok = false;
    CHECK(ok);
}
