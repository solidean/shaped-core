#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh> // sg::create_dx12_context
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// The test target declares this package itself (sc_add_shader_package in the CMakeLists); generated
// into the build dir and private to this binary.
#include <sg_test_shaders.hh>

// Render-routine framework tests, on a dx12 WARP context (software, present on any Windows host, so
// they run on headless CI). WARP + DXC are Windows-only, which is why this file is gated on the dx12
// backend and a shader compiler in the CMakeLists. Three things are proven here:
//   1. the framework's phase orchestration re-runs declare/materialize on a reload, but not init_once,
//   2. routines are per-context — a fresh context re-initializes from scratch (no stale cross-context
//      state, the bug the per-context registry design fixes by construction), and
//   3. a real routine compiles a compute shader through slib and dispatches it end to end.

namespace
{
// A fake routine that records how often each phase ran; does no GPU work.
class counting_routine : public sg::render_routine<counting_routine>
{
public:
    int once = 0;
    int declare = 0;
    int materialize = 0;

protected:
    void init_once(sg::context&) override { ++once; }
    void init_declare(sg::context&) override { ++declare; }
    void init_materialize(sg::command_list&) override { ++materialize; }
};

// The end-to-end routine: owns its pipeline via init_declare, dispatches in execute. Reached by type —
// no handle, no registration call.
class pattern_fill_routine : public sg::render_routine<pattern_fill_routine>
{
public:
    static void execute(sg::command_list& cmd, sg::buffer<sg::u32> const& out)
    {
        auto const& self = acquire(cmd);
        CC_ASSERT(self._pipeline != nullptr, "pattern_fill routine failed to initialize");

        // Force the compute pipeline only now — init_declare merely kicked off the background compile.
        auto const pipeline = cc::async_blocking_get_singlethreaded(self._pipeline);

        auto const group = cmd.context().transient.create_binding_group(
            self._group_layout, {{.name = "gValues", .view = out.as_readwrite_buffer()}});

        cmd.compute.bind_pipeline(*pipeline);
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(out.element_count());
    }

protected:
    void init_declare(sg::context& ctx) override
    {
        auto const shader = sg::test::shaders::pattern_fill.compute.main->acquire(ctx);
        (void)cc::try_async_blocking_get_singlethreaded(shader); // no async pool here, so drive it inline
        auto const* const compiled = shader->try_value();
        if (compiled == nullptr)
            return; // the context cannot produce a format we can use; execute() then asserts

        _group_layout = ctx.cached.acquire_binding_group_layout(compiled->bindings);
        auto const layout
            = ctx.cached.acquire_pipeline_layout(sg::pipeline_layout_description{.groups = {_group_layout}});
        // Only kick off the background compile here — execute() forces it when it actually needs the pipeline.
        _pipeline = ctx.cached.acquire_compute_pipeline(
            sg::compute_pipeline_description{.shader = *compiled, .layout = layout});
    }

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};

// A dx12 WARP context, or nullptr where none is available (the caller SKIPs).
sg::context_handle make_warp_context()
{
    auto ctx = sg::create_dx12_context({.enable_debug_layer = true, .use_warp = true});
    return ctx.has_value() ? ctx.value() : nullptr;
}
} // namespace

TEST("sg - routine phases run once, then re-run declare + materialize on a reload")
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto cmd = ctx->create_command_list();

    auto const& routine = counting_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 1);
    CHECK(routine.materialize == 1);

    // A second pass at the same generation changes nothing (same per-context instance). We re-read
    // through `routine`, so the returned reference is intentionally discarded.
    (void)counting_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 1);
    CHECK(routine.materialize == 1);

    // A reload bumps the global generation: declare + materialize re-run, init_once does not.
    sg::signal_reload();
    (void)counting_routine::acquire(*cmd);
    CHECK(routine.once == 1);
    CHECK(routine.declare == 2);
    CHECK(routine.materialize == 2);

    ctx->drop_command_list(cc::move(cmd));
}

TEST("sg - routines are per-context: each context builds its own instance from scratch")
{
    // Context A initializes the routine, then goes away.
    {
        auto const ctx_a = make_warp_context();
        if (ctx_a == nullptr)
            SKIP("no dx12 WARP device");

        auto cmd_a = ctx_a->create_command_list();
        auto const& ra = counting_routine::acquire(*cmd_a);
        CHECK(ra.once == 1);
        CHECK(ra.declare == 1);
        CHECK(ra.materialize == 1);
        ctx_a->drop_command_list(cc::move(cmd_a));
    } // ctx_a shuts down here — its routine instance (and cached GPU state) is released with it.

    // Advance the global generation. On A's instance this would only bump declare/materialize; a fresh
    // context must instead build its OWN instance from scratch — init_once included.
    sg::signal_reload();

    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_b = ctx_b->create_command_list();
    auto const& rb = counting_routine::acquire(*cmd_b);
    CHECK(rb.once == 1); // ran again on ctx_b: the instance is per-context, not a process singleton
    CHECK(rb.declare == 1);
    CHECK(rb.materialize == 1);
    ctx_b->drop_command_list(cc::move(cmd_b));
}

TEST("sg - evicting a routine drops its instance (the acquire cache does not resurrect it)")
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");

    auto cmd = ctx->create_command_list();

    auto const& first = counting_routine::acquire(*cmd);
    CHECK(first.once == 1);

    // Drive the first instance's declare count to 2, so it is distinguishable from a fresh one.
    sg::signal_reload();
    (void)counting_routine::acquire(*cmd);
    CHECK(first.declare == 2);

    counting_routine::evict(*ctx);

    // A fresh instance, built from scratch: every phase back at 1. A cached slot that survived the
    // eviction would instead hand back the old object (declare == 2) — or worse, a freed one.
    auto const& second = counting_routine::acquire(*cmd);
    CHECK(second.once == 1);
    CHECK(second.declare == 1);
    CHECK(second.materialize == 1);

    ctx->drop_command_list(cc::move(cmd));
}

TEST("sg - two live contexts keep separate routine instances")
{
    auto const ctx_a = make_warp_context();
    if (ctx_a == nullptr)
        SKIP("no dx12 WARP device");
    auto const ctx_b = make_warp_context();
    REQUIRE(ctx_b != nullptr);

    auto cmd_a = ctx_a->create_command_list();
    auto cmd_b = ctx_b->create_command_list();

    auto const& ra = counting_routine::acquire(*cmd_a);
    auto const& rb = counting_routine::acquire(*cmd_b);
    CHECK(&ra != &rb);

    // Interleaved acquires must keep landing on the right instance — neither context may be served the
    // other's routine, however the per-thread acquire cache ping-pongs between them.
    (void)counting_routine::acquire(*cmd_a);
    (void)counting_routine::acquire(*cmd_b);
    (void)counting_routine::acquire(*cmd_a);

    CHECK(ra.once == 1);
    CHECK(ra.declare == 1);
    CHECK(rb.once == 1);
    CHECK(rb.declare == 1);

    ctx_a->drop_command_list(cc::move(cmd_a));
    ctx_b->drop_command_list(cc::move(cmd_b));
}

TEST("sg - a routine compiles a shader and dispatches it end to end")
{
    auto const ctx = make_warp_context();
    if (ctx == nullptr)
        SKIP("no dx12 WARP device");
    if (!ctx->accepts_shader_format(sg::shader_format::dxil))
        SKIP("context does not accept DXIL");

    slib::shader_library shader_lib;
    auto compiler = slib::create_dxc_compiler();
    REQUIRE(compiler.has_value());
    shader_lib.add_compiler(cc::move(compiler.value()));
    shader_lib.add_package(sg::test::shaders::package());

    constexpr int count = 256; // a multiple of the shader's 64-thread workgroup
    auto const out
        = ctx->persistent.create_buffer<sg::u32>(count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    REQUIRE(out.raw() != nullptr);

    auto disp = ctx->create_command_list();
    pattern_fill_routine::execute(*disp, out);
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
